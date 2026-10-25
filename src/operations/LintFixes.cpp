#include "LSP/LintFixes.hpp"
#include "LSP/Destructure.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <type_traits>

namespace Luau::LanguageServer::LintFixes
{
namespace
{

Luau::AstExpr* unwrapGroup(Luau::AstExpr* expr)
{
    while (auto group = expr->as<Luau::AstExprGroup>())
        expr = group->expr;
    return expr;
}

// The node of type T spanning exactly `location`. Types are searched too, since some lints report on annotations.
template<typename T>
struct FindNodeAt : Luau::AstVisitor
{
    Luau::Location location;
    T* result = nullptr;

    explicit FindNodeAt(const Luau::Location& location)
        : location(location)
    {
    }

    bool visit(Luau::AstNode* node) override
    {
        if (!result && node->location == location)
        {
            // The base classes have no RTTI index of their own
            if constexpr (std::is_same_v<T, Luau::AstExpr>)
                result = node->asExpr();
            else if constexpr (std::is_same_v<T, Luau::AstStat>)
                result = node->asStat();
            else
                result = node->as<T>();
        }
        return !result;
    }

    bool visit(Luau::AstType* node) override
    {
        return visit(static_cast<Luau::AstNode*>(node));
    }

    bool visit(Luau::AstTypePack* node) override
    {
        return visit(static_cast<Luau::AstNode*>(node));
    }
};

// Every `if`/`while`/if-else clause list in the module, for the lints about `when` chains and bindings
struct ClauseVisitor : Luau::AstVisitor
{
    std::function<void(const Luau::AstArray<Luau::AstIfClause>&)> onClauses;

    bool visit(Luau::AstStatIf* node) override
    {
        onClauses(node->clauses);
        return true;
    }

    bool visit(Luau::AstStatWhile* node) override
    {
        onClauses(node->clauses);
        return true;
    }

    bool visit(Luau::AstExprIfElse* node) override
    {
        onClauses(node->clauses);
        return true;
    }
};

struct FixContext
{
    Luau::AstStatBlock* root;
    const TextDocument& textDocument;

    std::string text(const Luau::Location& location) const
    {
        return textDocument.getText(textDocument.convertLocation(location));
    }

    std::string text(Luau::AstNode* node) const
    {
        return text(node->location);
    }

    lsp::TextEdit replace(const Luau::Location& location, std::string newText) const
    {
        return {textDocument.convertLocation(location), std::move(newText)};
    }

    template<typename T>
    T* find(const Luau::Location& location) const
    {
        FindNodeAt<T> finder{location};
        root->visit(&finder);
        return finder.result;
    }

    // The first `token` between `from` and `to` outside comments: an operator, which the AST doesn't keep the location of
    std::optional<Luau::Location> findToken(const Luau::Position& from, const Luau::Position& to, std::string_view token) const
    {
        std::string between = text(Luau::Location{from, to});
        Luau::Position position = from;

        for (size_t i = 0; i < between.size();)
        {
            if (between.compare(i, 2, "--") == 0)
            {
                // A long comment `--[==[ ... ]==]`, or a line comment
                size_t level = 0;
                bool longComment = i + 2 < between.size() && between[i + 2] == '[';
                for (size_t j = i + 3; longComment && j < between.size() && between[j] != '['; ++j)
                {
                    if (between[j] != '=')
                        longComment = false;
                    else
                        ++level;
                }

                std::string close = longComment ? "]" + std::string(level, '=') + "]" : "\n";
                size_t end = between.find(close, i + 2);
                end = end == std::string::npos ? between.size() : end + close.size();
                for (; i < end; ++i)
                    advance(position, between[i]);
                continue;
            }

            if (between.compare(i, token.size(), token) == 0)
                return Luau::Location{position, Luau::Position{position.line, position.column + unsigned(token.size())}};

            advance(position, between[i]);
            ++i;
        }

        return std::nullopt;
    }

    static void advance(Luau::Position& position, char c)
    {
        if (c == '\n')
            position = Luau::Position{position.line + 1, 0};
        else
            position.column++;
    }
};

LintFix singleEdit(std::string title, lsp::TextEdit edit, bool preferred = true)
{
    return LintFix{std::move(title), {std::move(edit)}, preferred};
}

// NilNoneComparison: `x == nil` where `x` can only be `none`, or the reverse
std::optional<LintFix> fixNilNoneComparison(const Luau::LintWarning& lint, const FixContext& context)
{
    auto comparison = context.find<Luau::AstExprBinary>(lint.location);
    if (!comparison)
        return std::nullopt;

    auto isSentinel = [](Luau::AstExpr* expr)
    {
        auto global = expr->as<Luau::AstExprGlobal>();
        return expr->is<Luau::AstExprConstantNil>() || (global && global->name == "none");
    };

    Luau::AstExpr* sentinel = isSentinel(comparison->left) ? comparison->left : comparison->right;
    if (!isSentinel(sentinel))
        return std::nullopt;

    const char* instead = sentinel->is<Luau::AstExprConstantNil>() ? "none" : "nil";
    return singleEdit(std::string("Compare with '") + instead + "'", context.replace(sentinel->location, instead));
}

// VarargCast: `f(... :: T)` passes only the first value. Parentheses keep that behavior and say so.
std::optional<LintFix> fixVarargCast(const Luau::LintWarning& lint, const FixContext& context)
{
    auto cast = context.find<Luau::AstExprTypeAssertion>(lint.location);
    if (!cast)
        return std::nullopt;

    return singleEdit("Wrap in parentheses to keep only the first value", context.replace(cast->location, "(" + context.text(cast) + ")"), false);
}

// StringIndexZero: `string.byte(s, 0)` reads before the first character. `sub` treats 0 as 1 already, and the index that's
// wrong there is the end one, so it gets no fix.
std::optional<LintFix> fixStringIndexZero(const Luau::LintWarning& lint, const FixContext& context)
{
    struct FindByteCall : Luau::AstVisitor
    {
        Luau::Location location;
        bool found = false;

        bool visit(Luau::AstExprCall* node) override
        {
            auto function = node->func->as<Luau::AstExprIndexName>();
            if (function && function->index == "byte")
                for (auto arg : node->args)
                    found = found || arg->location == location;
            return !found;
        }
    };

    FindByteCall finder;
    finder.location = lint.location;
    context.root->visit(&finder);
    if (!finder.found)
        return std::nullopt;

    return singleEdit("Read the first character at index 1", context.replace(lint.location, "1"));
}

// TableTruthiness: `if t then` and `if not t then` on a table. A binding's value (`if local y = t then`) is left alone,
// since rewriting it would change what gets bound.
std::optional<LintFix> fixTableTruthiness(const Luau::LintWarning& lint, const FixContext& context)
{
    auto condition = context.find<Luau::AstExpr>(lint.location);
    if (!condition)
        return std::nullopt;

    bool bindingValue = false;
    ClauseVisitor clauses;
    clauses.onClauses = [&](const Luau::AstArray<Luau::AstIfClause>& list)
    {
        for (const auto& clause : list)
            bindingValue = bindingValue || (clause.local && clause.expr && clause.expr->location == lint.location);
    };
    context.root->visit(&clauses);
    if (bindingValue)
        return std::nullopt;

    if (auto negation = condition->as<Luau::AstExprUnary>(); negation && negation->op == Luau::AstExprUnary::Op::Not)
    {
        std::string tested = context.text(unwrapGroup(negation->expr));
        return singleEdit("Check whether the table is empty", context.replace(lint.location, "next(" + tested + ") == nil"));
    }

    return singleEdit("Check whether the table has entries", context.replace(lint.location, "next(" + context.text(condition) + ") ~= nil"));
}

// DiscardedResult: a pure builtin or `@nodiscard` function called as a statement. Discarding it on purpose is the fix
// that applies to every one; a string method gets a second fix that keeps its result.
std::vector<LintFix> fixDiscardedResult(const Luau::LintWarning& lint, const FixContext& context)
{
    auto call = context.find<Luau::AstExprCall>(lint.location);
    if (!call)
        return {};

    std::vector<LintFix> fixes;
    fixes.push_back(singleEdit("Discard the result with 'const _ ='", context.replace(Luau::Location{call->location.begin, 0}, "const _ = "), false));

    auto method = call->self ? call->func->as<Luau::AstExprIndexName>() : nullptr;
    Luau::AstExpr* receiver = method ? unwrapGroup(method->expr) : nullptr;
    if (receiver && (receiver->is<Luau::AstExprLocal>() || receiver->is<Luau::AstExprGlobal>() || receiver->is<Luau::AstExprIndexName>()))
    {
        std::string name = context.text(receiver);
        fixes.push_back(
            singleEdit("Assign the result back to '" + name + "'", context.replace(Luau::Location{call->location.begin, 0}, name + " = "), false));
    }

    return fixes;
}

// FloatIndex: `xs[#xs / 2]`, where `//` gives the integer index meant
std::optional<LintFix> fixFloatIndex(const Luau::LintWarning& lint, const FixContext& context)
{
    auto division = context.find<Luau::AstExprBinary>(lint.location);
    if (!division || division->op != Luau::AstExprBinary::Div)
        return std::nullopt;

    auto slash = context.findToken(division->left->location.end, division->right->location.begin, "/");
    if (!slash)
        return std::nullopt;

    return singleEdit("Divide to an integer with '//'", context.replace(*slash, "//"));
}

// Pairs/Ipairs: `for k, v in pairs(t) do`, which generalized iteration writes as `for k, v in t do`. Any other use of
// `pairs` (passing it around, calling it outside a loop) gets no fix.
std::optional<LintFix> fixLuaIterator(const Luau::LintWarning& lint, const FixContext& context, const char* name)
{
    struct FindIteratorCall : Luau::AstVisitor
    {
        Luau::Location location;
        Luau::AstExprCall* result = nullptr;

        bool visit(Luau::AstStatForIn* node) override
        {
            auto call = node->values.size == 1 ? node->values.data[0]->as<Luau::AstExprCall>() : nullptr;
            if (call && call->args.size == 1 && call->func->is<Luau::AstExprGlobal>() && call->func->location == location)
                result = call;
            return !result;
        }
    };

    FindIteratorCall finder;
    finder.location = lint.location;
    context.root->visit(&finder);
    if (!finder.result)
        return std::nullopt;

    return singleEdit(std::string("Iterate directly instead of with '") + name + "'",
        context.replace(finder.result->location, context.text(finder.result->args.data[0])));
}

// The statement `stat` deleted with its lines when nothing else is on them, or just its text
lsp::TextEdit deleteStatement(const FixContext& context, Luau::AstStat* stat)
{
    const Luau::Location& location = stat->location;
    std::string before = context.text(Luau::Location{{location.begin.line, 0}, location.begin});
    std::string after = context.text(Luau::Location{location.end, {location.end.line + 1, 0}});

    bool blank = [](const std::string& s)
    {
        return s.find_first_not_of(" \t\r\n;") == std::string::npos;
    }(before + after);

    if (blank)
        return {context.textDocument.convertLocation(Luau::Location{{location.begin.line, 0}, {location.end.line + 1, 0}}), ""};
    return context.replace(location, "");
}

// SelfAssignment: `x = x or return` (keep the guard), `x = x` (delete it) and `if const s = s then` (test `s` directly)
std::optional<LintFix> fixSelfAssignment(const Luau::LintWarning& lint, const FixContext& context)
{
    if (auto assign = context.find<Luau::AstStatAssign>(lint.location); assign && assign->vars.size == 1 && assign->values.size == 1)
    {
        Luau::AstExpr* value = assign->values.data[0];
        auto guard = value->as<Luau::AstExprBinary>();
        if (guard && guard->op == Luau::AstExprBinary::Or && unwrapGroup(guard->right)->is<Luau::AstExprDo>())
            return singleEdit("Keep only the guard", context.replace(assign->location, context.text(guard)));

        return singleEdit("Remove the self-assignment", deleteStatement(context, assign));
    }

    // A binding to itself: the warning spans from the bound name to its value
    std::optional<LintFix> result;
    ClauseVisitor clauses;
    clauses.onClauses = [&](const Luau::AstArray<Luau::AstIfClause>& list)
    {
        for (const auto& clause : list)
        {
            if (!clause.local || !clause.expr || !clause.declaration || clause.local->location.begin != lint.location.begin ||
                clause.expr->location.end != lint.location.end)
                continue;

            Luau::Position start = clause.declaration->keywordLocation ? clause.declaration->keywordLocation->begin : clause.local->location.begin;
            std::string name = clause.local->name.value;
            result = singleEdit("Test '" + name + "' directly", context.replace(Luau::Location{start, clause.expr->location.end}, name));
        }
    };
    context.root->visit(&clauses);
    return result;
}

// Whether `expr` written as `a // b` instead of a call needs parentheses where it is: as an operand of something that
// binds tighter than `//`, or of `*`, `/`, `//` or `%` on their right.
bool needsParenthesesAsFloorDivision(Luau::AstStatBlock* root, Luau::AstExpr* expr)
{
    struct FindParent : Luau::AstVisitor
    {
        Luau::AstExpr* expr;
        bool result = false;

        bool visit(Luau::AstExprUnary* node) override
        {
            result = result || node->expr == expr;
            return true;
        }

        bool visit(Luau::AstExprBinary* node) override
        {
            bool sameLevel = node->op == Luau::AstExprBinary::Mul || node->op == Luau::AstExprBinary::Div ||
                             node->op == Luau::AstExprBinary::FloorDiv || node->op == Luau::AstExprBinary::Mod;
            result =
                result || (node->op == Luau::AstExprBinary::Pow && (node->left == expr || node->right == expr)) || (sameLevel && node->right == expr);
            return true;
        }

        bool visit(Luau::AstExprIndexName* node) override
        {
            result = result || node->expr == expr;
            return true;
        }

        bool visit(Luau::AstExprIndexExpr* node) override
        {
            result = result || node->expr == expr;
            return true;
        }

        bool visit(Luau::AstExprCall* node) override
        {
            result = result || node->func == expr;
            return true;
        }

        bool visit(Luau::AstExprTypeAssertion* node) override
        {
            result = result || node->expr == expr;
            return true;
        }
    };

    FindParent finder;
    finder.expr = expr;
    root->visit(&finder);
    return finder.result;
}

// FloorDivision: `math.floor(a / b)` is `a // b`. The operands keep their text, comments included.
std::optional<LintFix> fixFloorDivision(const Luau::LintWarning& lint, const FixContext& context)
{
    auto call = context.find<Luau::AstExprCall>(lint.location);
    auto division = call && call->args.size == 1 ? unwrapGroup(call->args.data[0])->as<Luau::AstExprBinary>() : nullptr;
    if (!division || division->op != Luau::AstExprBinary::Div)
        return std::nullopt;

    auto slash = context.findToken(division->left->location.end, division->right->location.begin, "/");
    if (!slash)
        return std::nullopt;

    std::string replacement = context.text(Luau::Location{division->left->location.begin, slash->begin}) + "//" +
                              context.text(Luau::Location{slash->end, division->right->location.end});
    if (needsParenthesesAsFloorDivision(context.root, call))
        replacement = "(" + replacement + ")";

    return singleEdit("Replace with '//'", context.replace(call->location, replacement));
}

// DiscardedValues: a call in an if-else branch keeps only its first value. Parentheses keep that behavior and say so.
std::optional<LintFix> fixDiscardedValues(const Luau::LintWarning& lint, const FixContext& context)
{
    auto call = context.find<Luau::AstExprCall>(lint.location);
    if (!call)
        return std::nullopt;

    return singleEdit("Wrap in parentheses to keep only the first value", context.replace(call->location, "(" + context.text(call) + ")"), false);
}

// Luwu String Extensions: the call or comparison a LuaStringHelper warning is on, and the `x == nil` / `x ~= nil` around it
// when the warning is on a search used as a condition
struct FindLuaStringHelperTarget : Luau::AstVisitor
{
    Luau::Location location;
    Luau::AstExpr* result = nullptr;
    Luau::AstExprBinary* nilComparison = nullptr;

    explicit FindLuaStringHelperTarget(const Luau::Location& location)
        : location(location)
    {
    }

    bool visit(Luau::AstExpr* node) override
    {
        if (node->location == location && (node->is<Luau::AstExprCall>() || node->is<Luau::AstExprBinary>()))
            result = node;
        return !result;
    }

    bool visit(Luau::AstExprBinary* node) override
    {
        if (node->op == Luau::AstExprBinary::CompareEq || node->op == Luau::AstExprBinary::CompareNe)
        {
            Luau::AstExpr* left = unwrapGroup(node->left);
            Luau::AstExpr* right = unwrapGroup(node->right);
            if ((left->location == location && right->is<Luau::AstExprConstantNil>()) ||
                (right->location == location && left->is<Luau::AstExprConstantNil>()))
                nilComparison = node;
        }
        return visit(static_cast<Luau::AstExpr*>(node));
    }

    static Luau::AstExpr* unwrapGroup(Luau::AstExpr* expr)
    {
        while (auto group = expr->as<Luau::AstExprGroup>())
            expr = group->expr;
        return expr;
    }
};

// Luwu String Extensions: rewrites what LuaStringHelper reports into the builtin it suggests. The lint's message quotes
// the replacement, but shortens long expressions to `...`, so the edit is built again here from the source text.
class LuaStringHelperRewriter
{
public:
    LuaStringHelperRewriter(const TextDocument& textDocument)
        : textDocument(textDocument)
    {
    }

    struct Rewrite
    {
        std::string text;
        std::string builtin;
        // Another library's helper may do more than the builtin (seal's trims take a pattern), so it isn't preferred
        bool preferred = true;
        // A `find` or `match` the lint reports because only whether it found something is read
        bool search = false;
    };

    // The new text for `target`, and the builtin it calls
    std::optional<Rewrite> rewrite(Luau::AstExpr* target)
    {
        if (auto comparison = target->as<Luau::AstExprBinary>())
            return rewriteSubComparison(comparison);
        if (auto call = target->as<Luau::AstExprCall>())
            return rewriteCall(call);
        return std::nullopt;
    }

private:
    const TextDocument& textDocument;

    struct StringCall
    {
        Luau::AstExprCall* call;
        std::string name;
        Luau::AstExpr* subject;
        // The arguments after the subject
        std::vector<Luau::AstExpr*> args;
    };

    std::string text(Luau::AstExpr* expr) const
    {
        return textDocument.getText(textDocument.convertLocation(expr->location));
    }

    static std::optional<StringCall> matchCall(Luau::AstExpr* expr)
    {
        auto call = FindLuaStringHelperTarget::unwrapGroup(expr)->as<Luau::AstExprCall>();
        auto function = call ? call->func->as<Luau::AstExprIndexName>() : nullptr;
        if (!function || (!call->self && call->args.size == 0))
            return std::nullopt;

        StringCall result{call, function->index.value, call->self ? function->expr : call->args.data[0], {}};
        for (size_t i = call->self ? 0 : 1; i < call->args.size; ++i)
            result.args.push_back(call->args.data[i]);
        return result;
    }

    static bool isStringLibrary(const StringCall& call)
    {
        auto library = call.call->self ? nullptr : call.call->func->as<Luau::AstExprIndexName>()->expr->as<Luau::AstExprGlobal>();
        return call.call->self || (library && library->name == "string");
    }

    static std::optional<std::string_view> constantString(Luau::AstExpr* expr)
    {
        auto string = expr ? FindLuaStringHelperTarget::unwrapGroup(expr)->as<Luau::AstExprConstantString>() : nullptr;
        if (!string)
            return std::nullopt;
        return std::string_view(string->value.data, string->value.size);
    }

    static bool isNumber(Luau::AstExpr* expr, double value)
    {
        auto number = expr ? FindLuaStringHelperTarget::unwrapGroup(expr)->as<Luau::AstExprConstantNumber>() : nullptr;
        return number && number->value == value;
    }

    static bool isNegativeNumber(Luau::AstExpr* expr, double value)
    {
        auto minus = expr ? FindLuaStringHelperTarget::unwrapGroup(expr)->as<Luau::AstExprUnary>() : nullptr;
        if (minus && minus->op == Luau::AstExprUnary::Op::Minus)
            return isNumber(minus->expr, value);
        return isNumber(expr, -value);
    }

    // `"lit"`, escaping what a Luau string needs
    static std::string stringLiteral(std::string_view value)
    {
        std::string result = "\"";
        for (char c : value)
        {
            if (c == '"' || c == '\\')
                result += std::string("\\") + c;
            else if (c == '\n')
                result += "\\n";
            else if (c == '\r')
                result += "\\r";
            else if (c == '\t')
                result += "\\t";
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                char escape[5];
                snprintf(escape, sizeof(escape), "\\x%02X", static_cast<unsigned char>(c));
                result += escape;
            }
            else
                result += c;
        }
        return result + "\"";
    }

    // A pattern with no magic characters, unescaped: `%.luwu` is `.luwu`
    static std::optional<std::string> literalPattern(std::string_view pattern)
    {
        std::string literal;
        for (size_t i = 0; i < pattern.size(); ++i)
        {
            char c = pattern[i];
            if (c == '%')
            {
                if (i + 1 >= pattern.size() || isalnum(static_cast<unsigned char>(pattern[i + 1])))
                    return std::nullopt;
                literal += pattern[++i];
            }
            else if (strchr("^$*+?.([])-", c))
                return std::nullopt;
            else
                literal += c;
        }
        return literal;
    }

    // `s:builtin(args)`, or `string.builtin(s, args)` when a method call can't follow the subject as written, the same
    // choice the lint's message makes
    std::string suggestion(Luau::AstExpr* subject, const std::string& builtin, const std::vector<std::string>& args) const
    {
        Luau::AstExpr* inner = FindLuaStringHelperTarget::unwrapGroup(subject);
        bool takesMethodCall = inner->is<Luau::AstExprLocal>() || inner->is<Luau::AstExprGlobal>() || inner->is<Luau::AstExprIndexName>() ||
                               inner->is<Luau::AstExprIndexExpr>() || inner->is<Luau::AstExprCall>();
        if (takesMethodCall)
            return text(inner) + ":" + builtin + "(" + join(args) + ")";

        std::vector<std::string> allArgs{text(subject)};
        allArgs.insert(allArgs.end(), args.begin(), args.end());
        return "string." + builtin + "(" + join(allArgs) + ")";
    }

    static std::string join(const std::vector<std::string>& parts)
    {
        std::string result;
        for (const auto& part : parts)
            result += (result.empty() ? "" : ", ") + part;
        return result;
    }

    // `sub(s, 1, n) == lit`, `sub(s, 1, #p) == p`, `sub(s, -n) == lit` and `sub(s, -#p) == p`. The lint only reports the
    // ones that check a prefix or suffix, so which side holds the `sub` and whether it takes one or two numbers is all
    // that's left to tell apart.
    std::optional<Rewrite> rewriteSubComparison(Luau::AstExprBinary* comparison)
    {
        for (auto [side, needle] : {std::pair{comparison->left, comparison->right}, std::pair{comparison->right, comparison->left}})
        {
            auto sub = matchCall(side);
            if (!sub || sub->name != "sub" || !isStringLibrary(*sub) || sub->args.empty())
                continue;

            std::string builtin;
            if (sub->args.size() == 2 && isNumber(sub->args[0], 1))
                builtin = "startswith";
            else if (sub->args.size() == 1)
                builtin = "endswith";
            else
                continue;

            std::string replacement = suggestion(sub->subject, builtin, {text(needle)});
            if (comparison->op == Luau::AstExprBinary::CompareNe)
                replacement = "not " + replacement;
            return Rewrite{replacement, builtin};
        }
        return std::nullopt;
    }

    std::optional<Rewrite> rewriteCall(Luau::AstExprCall* node)
    {
        auto call = matchCall(node);
        if (!call)
            return std::nullopt;

        if (!isStringLibrary(*call))
            return rewriteHelperCall(*call);

        auto pattern = constantString(call->args.empty() ? nullptr : call->args[0]);

        // `find(s, x, 1, true)`
        if (call->name == "find" && call->args.size() == 3)
            return Rewrite{suggestion(call->subject, "contains", {text(call->args[0])}), "contains", true, /* search= */ true};

        if (!pattern)
            return std::nullopt;

        if (call->name == "match" && call->args.size() == 1 && *pattern == "^%s*(.-)%s*$")
            return Rewrite{suggestion(call->subject, "trim", {}), "trim"};

        // An anchored literal pattern given to `match` or `find`
        if ((call->name == "match" || call->name == "find") && call->args.size() == 1 && pattern->size() > 1)
        {
            bool anchoredStart = pattern->front() == '^';
            std::string_view body = anchoredStart ? pattern->substr(1) : pattern->substr(0, pattern->size() - 1);
            auto literal = literalPattern(body);
            if (!literal)
                return std::nullopt;

            std::string builtin = anchoredStart ? "startswith" : "endswith";
            return Rewrite{suggestion(call->subject, builtin, {stringLiteral(*literal)}), builtin, true, /* search= */ true};
        }

        if (call->name != "gsub" || call->args.size() < 2)
            return std::nullopt;

        bool trimsStart = *pattern == "^%s+" || *pattern == "^%s*";
        bool trimsEnd = *pattern == "%s+$" || *pattern == "%s*$";
        if (trimsStart || trimsEnd)
        {
            std::string builtin = trimsStart ? "trimstart" : "trimend";
            return Rewrite{suggestion(call->subject, builtin, {}), builtin};
        }

        // A gsub whose pattern has no magic characters. The replacement has no `%` either, so it's kept as written.
        auto literal = literalPattern(*pattern);
        if (!literal)
            return std::nullopt;

        std::vector<std::string> args{stringLiteral(*literal), text(call->args[1])};
        if (call->args.size() > 2)
            args.push_back(text(call->args[2]));
        return Rewrite{suggestion(call->subject, "replace", args), "replace"};
    }

    // `str.startswith(s, p)`: another library's version of a builtin
    std::optional<Rewrite> rewriteHelperCall(const StringCall& call)
    {
        static const std::pair<const char*, const char*> kHelpers[] = {
            {"startswith", "startswith"},
            {"starts_with", "startswith"},
            {"startsWith", "startswith"},
            {"starts", "startswith"},
            {"endswith", "endswith"},
            {"ends_with", "endswith"},
            {"endsWith", "endswith"},
            {"ends", "endswith"},
            {"trim", "trim"},
            {"strip", "trim"},
            {"trimstart", "trimstart"},
            {"trim_start", "trimstart"},
            {"trimStart", "trimstart"},
            {"trimfront", "trimstart"},
            {"ltrim", "trimstart"},
            {"lstrip", "trimstart"},
            {"trimend", "trimend"},
            {"trim_end", "trimend"},
            {"trimEnd", "trimend"},
            {"trimback", "trimend"},
            {"rtrim", "trimend"},
            {"rstrip", "trimend"},
        };

        for (const auto& [helper, builtin] : kHelpers)
        {
            if (call.name != helper)
                continue;

            std::vector<std::string> args;
            for (auto arg : call.args)
                args.push_back(text(arg));
            return Rewrite{suggestion(call.subject, builtin, args), builtin, /* preferred= */ false};
        }
        return std::nullopt;
    }
};

// Any, AnyAnnotation and AnyCast: `any` where `unknown` keeps type checking on. The code using the value may then need
// a type test, so the fix isn't preferred.
std::optional<LintFix> fixAny(const Luau::LintWarning& lint, const FixContext& context)
{
    Luau::Location any = lint.location;
    if (lint.code == Luau::LintWarning::Code_AnyCast)
    {
        auto cast = context.find<Luau::AstExprTypeAssertion>(lint.location);
        if (!cast)
            return std::nullopt;
        any = cast->annotation->location;
    }

    if (context.text(any) != "any")
        return std::nullopt;

    return singleEdit("Use 'unknown' instead of 'any'", context.replace(any, "unknown"), false);
}

// ReturnSelf: a trait method annotated as returning the trait, where `Self` keeps the class
std::optional<LintFix> fixReturnSelf(const Luau::LintWarning& lint, const FixContext& context)
{
    if (!context.find<Luau::AstTypeReference>(lint.location))
        return std::nullopt;

    return singleEdit("Return 'Self'", context.replace(lint.location, "Self"));
}

// ReturnOnNextLine: `x or return` with a value on the next line, which `return` takes. `(return)` returns nothing, and
// for a `do` expression the parentheses go around the whole expression, since a bare `return` in one has to be.
std::optional<LintFix> fixReturnOnNextLine(const Luau::LintWarning& lint, const FixContext& context)
{
    struct FindReturningDo : Luau::AstVisitor
    {
        Luau::Location location;
        Luau::AstExprDo* result = nullptr;
        Luau::AstStatReturn* leave = nullptr;

        bool visit(Luau::AstExprDo* node) override
        {
            auto last = node->body->body.size > 0 ? node->body->body.data[node->body->body.size - 1]->as<Luau::AstStatReturn>() : nullptr;
            if (last && last->list.size > 0 && last->list.data[0]->location == location)
            {
                result = node;
                leave = last;
            }
            return true;
        }
    };

    FindReturningDo finder;
    finder.location = lint.location;
    context.root->visit(&finder);
    if (!finder.result)
        return std::nullopt;

    if (finder.result->shorthand)
        return singleEdit("Return no value", context.replace(finder.leave->returnLocation, "(return)"), false);

    return LintFix{"Return no value",
        {context.replace(Luau::Location{finder.result->location.begin, 0}, "("),
            context.replace(Luau::Location{finder.leave->returnLocation.end, 0}, ")")},
        false};
}

// OrContinue: `x or continue` in a module with a variable named `continue`, where `do continue` continues the loop
std::optional<LintFix> fixOrContinue(const Luau::LintWarning& lint, const FixContext& context)
{
    if (context.text(lint.location) != "continue")
        return std::nullopt;

    return singleEdit("Continue the loop with 'or do continue'", context.replace(lint.location, "do continue"));
}

// BindingSwallowsCondition: `if const x = f() and x > 1`, where `when` tests the binding instead. For an `or` the
// condition reads differently afterwards, so that fix isn't preferred.
std::optional<LintFix> fixBindingSwallowsCondition(const Luau::LintWarning& lint, const FixContext& context)
{
    std::optional<LintFix> result;
    ClauseVisitor clauses;
    clauses.onClauses = [&](const Luau::AstArray<Luau::AstIfClause>& list)
    {
        for (const auto& clause : list)
        {
            auto binary = clause.declaration && clause.expr ? clause.expr->as<Luau::AstExprBinary>() : nullptr;
            if (!binary || binary->right->location != lint.location)
                continue;

            bool isAnd = binary->op == Luau::AstExprBinary::And;
            if (!isAnd && binary->op != Luau::AstExprBinary::Or)
                continue;

            if (auto op = context.findToken(binary->left->location.end, binary->right->location.begin, isAnd ? "and" : "or"))
                result = singleEdit("Test the binding with 'when'", context.replace(*op, "when"), isAnd);
        }
    };
    context.root->visit(&clauses);
    return result;
}

// The blocks of a statement that has them, in source order: an `if`'s branches (`elseif`s included), a loop's or
// function's body, or a `do` block itself
std::vector<Luau::AstStatBlock*> blocksOf(Luau::AstStat* stat)
{
    if (auto ifStat = stat->as<Luau::AstStatIf>())
    {
        std::vector<Luau::AstStatBlock*> blocks{ifStat->thenbody};
        if (auto elseif = ifStat->elsebody ? ifStat->elsebody->as<Luau::AstStatIf>() : nullptr)
        {
            auto rest = blocksOf(elseif);
            blocks.insert(blocks.end(), rest.begin(), rest.end());
        }
        else if (auto elseBlock = ifStat->elsebody ? ifStat->elsebody->as<Luau::AstStatBlock>() : nullptr)
            blocks.push_back(elseBlock);
        return blocks;
    }
    if (auto whileStat = stat->as<Luau::AstStatWhile>())
        return {whileStat->body};
    if (auto repeat = stat->as<Luau::AstStatRepeat>())
        return {repeat->body};
    if (auto forStat = stat->as<Luau::AstStatFor>())
        return {forStat->body};
    if (auto forIn = stat->as<Luau::AstStatForIn>())
        return {forIn->body};
    if (auto function = stat->as<Luau::AstStatFunction>())
        return {function->func->body};
    if (auto function = stat->as<Luau::AstStatLocalFunction>())
        return {function->func->body};
    if (auto block = stat->as<Luau::AstStatBlock>())
        return {block};
    return {};
}

std::string trim(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n;");
    if (begin == std::string::npos)
        return "";
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// OneLineStatement and OneLineGuard: a statement with a body written on one line, split with its header, each body
// statement, every `else`/`elseif` and its `end` (or `until`) on their own lines
std::optional<LintFix> fixOneLineStatement(const Luau::LintWarning& lint, const FixContext& context)
{
    auto stat = context.find<Luau::AstStat>(lint.location);
    if (!stat)
        return std::nullopt;

    std::vector<Luau::AstStatBlock*> blocks = blocksOf(stat);
    if (blocks.empty())
        return std::nullopt;

    // A block in the middle with nothing in it leaves nothing to split around
    for (auto block : blocks)
        if (block->body.size == 0)
            return std::nullopt;

    std::string indent = Destructure::getLineIndentation(context.textDocument, stat->location.begin.line);
    std::string inner = indent + Destructure::detectIndentUnit(context.textDocument);

    std::string result = trim(context.text(Luau::Location{stat->location.begin, blocks[0]->body.data[0]->location.begin}));
    for (size_t b = 0; b < blocks.size(); ++b)
    {
        const auto& body = blocks[b]->body;
        for (size_t i = 0; i < body.size; ++i)
        {
            // A statement keeps what follows it up to the next one, a comment included
            Luau::Position end = i + 1 < body.size ? body.data[i + 1]->location.begin : body.data[i]->location.end;
            result += "\n" + inner + trim(context.text(Luau::Location{body.data[i]->location.begin, end}));
        }

        // The `else`/`elseif ... then` before the next block, or the `end`/`until` closing the statement
        Luau::Position next = b + 1 < blocks.size() ? blocks[b + 1]->body.data[0]->location.begin : stat->location.end;
        result += "\n" + indent + trim(context.text(Luau::Location{body.data[body.size - 1]->location.end, next}));
    }

    return singleEdit("Split over several lines", context.replace(stat->location, result));
}

// The expression a statement starting with `expr` starts with: a statement has to start with a name or `(`, and one
// starting with `(` can continue the line before it
bool startsWithName(Luau::AstExpr* expr)
{
    while (true)
    {
        if (auto binary = expr->as<Luau::AstExprBinary>())
            expr = binary->left;
        else if (auto call = expr->as<Luau::AstExprCall>())
            expr = call->func;
        else if (auto index = expr->as<Luau::AstExprIndexName>())
            expr = index->expr;
        else if (auto index = expr->as<Luau::AstExprIndexExpr>())
            expr = index->expr;
        else if (auto cast = expr->as<Luau::AstExprTypeAssertion>())
            expr = cast->expr;
        else
            return expr->is<Luau::AstExprLocal>() || expr->is<Luau::AstExprGlobal>();
    }
}

// LuaGuard: `if not x then return end` as `x or return;`. Only a condition whose negation can start a statement gets a
// fix: `not x`, `a == b` and `a ~= b`. `not (x > 1) or return` would start with `not`, which a statement can't.
std::optional<LintFix> fixLuaGuard(const Luau::LintWarning& lint, const FixContext& context)
{
    auto guard = context.find<Luau::AstStatIf>(lint.location);
    if (!guard || guard->elsebody || guard->clauses.size != 0 || guard->thenbody->body.size != 1)
        return std::nullopt;

    Luau::AstExpr* condition = unwrapGroup(guard->condition);
    std::string negated;
    Luau::AstExpr* start = nullptr;

    if (auto negation = condition->as<Luau::AstExprUnary>(); negation && negation->op == Luau::AstExprUnary::Op::Not)
    {
        start = unwrapGroup(negation->expr);
        negated = context.text(start);
    }
    else if (auto comparison = condition->as<Luau::AstExprBinary>();
        comparison && (comparison->op == Luau::AstExprBinary::CompareEq || comparison->op == Luau::AstExprBinary::CompareNe))
    {
        bool equal = comparison->op == Luau::AstExprBinary::CompareEq;
        auto op = context.findToken(comparison->left->location.end, comparison->right->location.begin, equal ? "==" : "~=");
        if (!op)
            return std::nullopt;

        start = comparison->left;
        negated = context.text(Luau::Location{comparison->left->location.begin, op->begin}) + (equal ? "~=" : "==") +
                  context.text(Luau::Location{op->end, comparison->right->location.end});
    }

    if (!start || !startsWithName(start))
        return std::nullopt;

    Luau::AstStat* leave = guard->thenbody->body.data[0];
    std::string leaveText;
    if (auto ret = leave->as<Luau::AstStatReturn>())
    {
        // A bare `return` needs the `;` so the next line isn't read as its value
        if (ret->list.size == 0)
            leaveText = "return;";
        else if (ret->list.size == 1)
            leaveText = context.text(ret);
        else
            leaveText = "(" + context.text(ret) + ")";
    }
    else if (leave->is<Luau::AstStatBreak>() || leave->is<Luau::AstStatContinue>())
        leaveText = context.text(leave);
    else
        return std::nullopt;

    std::string replacement = negated + " or " + leaveText;
    return singleEdit("Replace with '" + replacement + "'", context.replace(guard->location, replacement));
}

// InlineOrLeave: `or return` at the end of a long line, moved to its own line one level in
std::optional<LintFix> fixInlineOrLeave(const Luau::LintWarning& lint, const FixContext& context)
{
    struct FindOrLeave : Luau::AstVisitor
    {
        Luau::Location location;
        Luau::AstExprBinary* result = nullptr;

        bool visit(Luau::AstExprBinary* node) override
        {
            if (node->op == Luau::AstExprBinary::Or && unwrapGroup(node->right)->location == location)
                result = node;
            return !result;
        }
    };

    FindOrLeave finder;
    finder.location = lint.location;
    context.root->visit(&finder);
    if (!finder.result)
        return std::nullopt;

    Luau::AstExprBinary* orLeave = finder.result;
    auto op = context.findToken(orLeave->left->location.end, orLeave->right->location.begin, "or");
    if (!op)
        return std::nullopt;

    std::string indent = Destructure::getLineIndentation(context.textDocument, orLeave->left->location.begin.line) +
                         Destructure::detectIndentUnit(context.textDocument);
    return singleEdit("Move to its own line", context.replace(Luau::Location{orLeave->left->location.end, op->begin}, "\n" + indent));
}

// OrReturnComma: `x or return a, b`, where `b` is the next value, not a second one returned. Returning both and keeping
// the current meaning are both fixes; which one was meant is up to the code.
std::vector<LintFix> fixOrReturnComma(const Luau::LintWarning& lint, const FixContext& context)
{
    struct FindOrReturnItem : Luau::AstVisitor
    {
        Luau::Location location;
        Luau::AstExpr* item = nullptr;
        Luau::AstExprDo* leave = nullptr;
        Luau::AstExpr* next = nullptr;

        void check(const Luau::AstArray<Luau::AstExpr*>& list)
        {
            for (size_t i = 0; !item && i + 1 < list.size; ++i)
            {
                if (list.data[i + 1]->location != location)
                    continue;

                Luau::AstExpr* operand = nullptr;
                if (auto binary = list.data[i]->as<Luau::AstExprBinary>(); binary && binary->op == Luau::AstExprBinary::Or)
                    operand = binary->right;
                else if (auto ifElse = list.data[i]->as<Luau::AstExprIfElse>())
                    operand = ifElse->falseExpr;

                auto doExpr = operand ? operand->as<Luau::AstExprDo>() : nullptr;
                if (doExpr && doExpr->shorthand)
                {
                    item = list.data[i];
                    leave = doExpr;
                    next = list.data[i + 1];
                }
            }
        }

        bool visit(Luau::AstStatLocal* node) override
        {
            check(node->values);
            return true;
        }

        bool visit(Luau::AstStatAssign* node) override
        {
            check(node->values);
            return true;
        }

        bool visit(Luau::AstStatReturn* node) override
        {
            check(node->list);
            return true;
        }

        bool visit(Luau::AstExprCall* node) override
        {
            check(node->args);
            return true;
        }
    };

    FindOrReturnItem finder;
    finder.location = lint.location;
    context.root->visit(&finder);
    if (!finder.item)
        return {};

    auto insert = [&](const Luau::Position& at, const char* text)
    {
        return context.replace(Luau::Location{at, 0}, text);
    };

    return {
        LintFix{"Return both values", {insert(finder.leave->location.begin, "("), insert(finder.next->location.end, ")")}, false},
        LintFix{"Keep the 'return' to one value", {insert(finder.item->location.begin, "("), insert(finder.item->location.end, ")")}, false},
    };
}


// LuaStringHelper: a manual prefix/suffix check, substring search, whitespace trim or literal gsub, or another library's
// helper, replaced with the builtin. A helper the code defines itself gets no fix.
std::optional<LintFix> fixLuaStringHelper(const Luau::LintWarning& lint, const FixContext& context)
{
    FindLuaStringHelperTarget finder{lint.location};
    context.root->visit(&finder);
    if (!finder.result)
        return std::nullopt;

    LuaStringHelperRewriter rewriter{context.textDocument};
    auto rewritten = rewriter.rewrite(finder.result);
    if (!rewritten)
        return std::nullopt;

    std::string replacement = rewritten->text;
    Luau::Location range = lint.location;

    // A search compared to nil reads its result only as found or not found. A boolean compared to nil never equals it,
    // so the comparison goes too: `s:find(x, 1, true) == nil` is `not s:contains(x)`.
    if (rewritten->search && finder.nilComparison)
    {
        range = finder.nilComparison->location;
        if (finder.nilComparison->op == Luau::AstExprBinary::CompareEq)
            replacement = "not " + replacement;
    }

    return singleEdit("Use 'string." + rewritten->builtin + "'", context.replace(range, replacement), rewritten->preferred);
}

std::vector<LintFix> computeFixes(const Luau::LintWarning& lint, const FixContext& context)
{
    auto one = [](std::optional<LintFix> fix)
    {
        return fix ? std::vector<LintFix>{std::move(*fix)} : std::vector<LintFix>{};
    };

    switch (lint.code)
    {
    case Luau::LintWarning::Code_NilNoneComparison:
        return one(fixNilNoneComparison(lint, context));
    case Luau::LintWarning::Code_VarargCast:
        return one(fixVarargCast(lint, context));
    case Luau::LintWarning::Code_StringIndexZero:
        return one(fixStringIndexZero(lint, context));
    case Luau::LintWarning::Code_TableTruthiness:
        return one(fixTableTruthiness(lint, context));
    case Luau::LintWarning::Code_DiscardedResult:
        return fixDiscardedResult(lint, context);
    case Luau::LintWarning::Code_FloatIndex:
        return one(fixFloatIndex(lint, context));
    case Luau::LintWarning::Code_Pairs:
        return one(fixLuaIterator(lint, context, "pairs"));
    case Luau::LintWarning::Code_Ipairs:
        return one(fixLuaIterator(lint, context, "ipairs"));
    case Luau::LintWarning::Code_SelfAssignment:
        return one(fixSelfAssignment(lint, context));
    case Luau::LintWarning::Code_FloorDivision:
        return one(fixFloorDivision(lint, context));
    case Luau::LintWarning::Code_DiscardedValues:
        return one(fixDiscardedValues(lint, context));
    case Luau::LintWarning::Code_Any:
    case Luau::LintWarning::Code_AnyCast:
    case Luau::LintWarning::Code_AnyAnnotation:
        return one(fixAny(lint, context));
    case Luau::LintWarning::Code_ReturnSelf:
        return one(fixReturnSelf(lint, context));
    case Luau::LintWarning::Code_ReturnOnNextLine:
        return one(fixReturnOnNextLine(lint, context));
    case Luau::LintWarning::Code_OrContinue:
        return one(fixOrContinue(lint, context));
    case Luau::LintWarning::Code_BindingSwallowsCondition:
        return one(fixBindingSwallowsCondition(lint, context));
    case Luau::LintWarning::Code_OneLineStatement:
    case Luau::LintWarning::Code_OneLineGuard:
        return one(fixOneLineStatement(lint, context));
    case Luau::LintWarning::Code_LuaGuard:
        return one(fixLuaGuard(lint, context));
    case Luau::LintWarning::Code_InlineOrLeave:
        return one(fixInlineOrLeave(lint, context));
    case Luau::LintWarning::Code_OrReturnComma:
        return fixOrReturnComma(lint, context);
    case Luau::LintWarning::Code_LuaStringHelper:
        return one(fixLuaStringHelper(lint, context));
    default:
        return {};
    }
}

bool before(const lsp::Position& a, const lsp::Position& b)
{
    return a.line < b.line || (a.line == b.line && a.character < b.character);
}

bool overlaps(const lsp::Range& a, const lsp::Range& b)
{
    bool samePoint = a.start.line == b.start.line && a.start.character == b.start.character;
    return samePoint || (before(a.start, b.end) && before(b.start, a.end));
}

} // namespace

std::vector<LintFix> computeLintFixes(const Luau::LintWarning& lint, Luau::AstStatBlock* root, const TextDocument& textDocument)
{
    return computeFixes(lint, FixContext{root, textDocument});
}

std::optional<std::string> fixAllTitle(Luau::LintWarning::Code code)
{
    static const Luau::LintWarning::Code kFixable[] = {
        Luau::LintWarning::Code_NilNoneComparison,
        Luau::LintWarning::Code_VarargCast,
        Luau::LintWarning::Code_StringIndexZero,
        Luau::LintWarning::Code_TableTruthiness,
        Luau::LintWarning::Code_DiscardedResult,
        Luau::LintWarning::Code_FloatIndex,
        Luau::LintWarning::Code_Pairs,
        Luau::LintWarning::Code_Ipairs,
        Luau::LintWarning::Code_SelfAssignment,
        Luau::LintWarning::Code_FloorDivision,
        Luau::LintWarning::Code_DiscardedValues,
        Luau::LintWarning::Code_Any,
        Luau::LintWarning::Code_AnyCast,
        Luau::LintWarning::Code_AnyAnnotation,
        Luau::LintWarning::Code_ReturnSelf,
        Luau::LintWarning::Code_ReturnOnNextLine,
        Luau::LintWarning::Code_OrContinue,
        Luau::LintWarning::Code_BindingSwallowsCondition,
        Luau::LintWarning::Code_OneLineStatement,
        Luau::LintWarning::Code_OneLineGuard,
        Luau::LintWarning::Code_LuaGuard,
        Luau::LintWarning::Code_InlineOrLeave,
        Luau::LintWarning::Code_OrReturnComma,
        Luau::LintWarning::Code_LuaStringHelper,
    };

    if (std::find(std::begin(kFixable), std::end(kFixable), code) == std::end(kFixable))
        return std::nullopt;
    return std::string("Fix all '") + Luau::LintWarning::getName(code) + "' warnings in this module";
}

std::vector<lsp::TextEdit> computeFixAllEdits(
    Luau::LintWarning::Code code, const std::vector<Luau::LintWarning>& warnings, Luau::AstStatBlock* root, const TextDocument& textDocument)
{
    FixContext context{root, textDocument};

    // Each warning's first fix, in source order
    std::vector<std::vector<lsp::TextEdit>> fixes;
    for (const auto& lint : warnings)
    {
        if (lint.code != code)
            continue;
        auto computed = computeFixes(lint, context);
        if (!computed.empty() && !computed[0].edits.empty())
            fixes.push_back(std::move(computed[0].edits));
    }

    std::sort(fixes.begin(), fixes.end(),
        [](const auto& a, const auto& b)
        {
            return before(a[0].range.start, b[0].range.start);
        });

    std::vector<lsp::TextEdit> result;
    for (const auto& fix : fixes)
    {
        bool clashes = std::any_of(fix.begin(), fix.end(),
            [&](const lsp::TextEdit& edit)
            {
                return std::any_of(result.begin(), result.end(),
                    [&](const lsp::TextEdit& taken)
                    {
                        return overlaps(edit.range, taken.range);
                    });
            });

        if (!clashes)
            result.insert(result.end(), fix.begin(), fix.end());
    }
    return result;
}

} // namespace Luau::LanguageServer::LintFixes
