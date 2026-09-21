#include "LSP/KeywordHovers.hpp"

#include <fstream>
#include <unordered_map>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace
{

const std::unordered_map<std::string, std::string>& keywordHoverDatabase()
{
    static const std::unordered_map<std::string, std::string> database = []
    {
        std::unordered_map<std::string, std::string> result;

#ifdef LSP_KEYWORD_HOVERS_PATH
        std::ifstream file(LSP_KEYWORD_HOVERS_PATH);
        if (file)
        {
            json data;
            file >> data;
            if (data.is_object())
                for (const auto& [keyword, docs] : data.items())
                    if (docs.is_string())
                        result[keyword] = docs.get<std::string>();
        }
#endif

        return result;
    }();

    return database;
}

} // namespace

std::optional<std::string> getKeywordHoverDocs(const std::string& keyword)
{
    auto& database = keywordHoverDatabase();
    if (auto it = database.find(keyword); it != database.end())
        return it->second;
    return std::nullopt;
}

std::optional<KeywordHoverMatch> findKeywordDocKeyAtPosition(const std::vector<Luau::AstNode*>& ancestry, Luau::Position position)
{
    if (ancestry.empty())
        return std::nullopt;

    auto contains = [position](const Luau::Location& location)
    {
        return location.containsClosed(position);
    };
    auto match = [](std::string key, const Luau::Location& range) -> std::optional<KeywordHoverMatch>
    {
        return KeywordHoverMatch{std::move(key), range};
    };

    // A function statement's `functionLocation`/`keywordLocation` (and a class member's
    // qualifier/keyword locations) can never be reached through `node` alone: the AstExprFunction
    // body's own location spans the *entire* `function ... end` (matching or nearly matching its
    // wrapping AstStatFunction/AstStatLocalFunction's location), so findNodeOrTypeAtPosition always
    // prefers that inner, narrower-or-equal AstExprFunction over the wrapping statement -- and class
    // member qualifiers live on plain AstClassProperty/AstClassMethod structs that the AstVisitor
    // traversal never selects directly at all. So check both explicitly against `ancestry` (which,
    // unlike `node`, includes every enclosing node, not just the innermost match) before falling
    // back to the generic node-based checks below.
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
    {
        if (auto classStat = (*it)->as<Luau::AstStatClass>())
        {
            if (const auto* ctor = classStat->primaryConstructor)
            {
                // An access specifier written between the class's name and its primary
                // constructor's parameter list -- `class Particle private (...)` -- qualifies the
                // *constructor*, not the class and not any one field, so it gets its own doc
                // rather than reusing the plain `public`/`private` one, which would describe the
                // wrong thing entirely at this position.
                if (ctor->qualifierLocation && contains(*ctor->qualifierLocation))
                    return match(
                        ctor->visibility == Luau::AstClassMemberVisibility::Private ? "primary_constructor_private"
                                                                                    : "primary_constructor_public",
                        *ctor->qualifierLocation
                    );

                // Each parameter implicitly declares a field of the same name, so a specifier or
                // `const` written on a parameter describes that implicit field -- again close to,
                // but not the same as, what those keywords mean on a class body member.
                for (const auto& qualifiers : ctor->argsQualifiers)
                {
                    if (qualifiers.qualifierLocation && contains(*qualifiers.qualifierLocation))
                        return match(
                            qualifiers.visibility == Luau::AstClassMemberVisibility::Private ? "primary_constructor_param_private"
                                                                                             : "primary_constructor_param_public",
                            *qualifiers.qualifierLocation
                        );
                    if (qualifiers.constLocation && contains(*qualifiers.constLocation))
                        return match("primary_constructor_param_const", *qualifiers.constLocation);
                }
            }

            for (const auto& member : classStat->members)
            {
                if (auto prop = member.get_if<Luau::AstClassProperty>())
                {
                    if (prop->qualifierLocation && contains(*prop->qualifierLocation))
                        return match(
                            prop->visibility == Luau::AstClassMemberVisibility::Private ? "private" : "public", *prop->qualifierLocation
                        );
                    // A class field's `const` means "immutable after construction", not "can't be
                    // reassigned in this scope" -- worth its own doc, distinct from a local `const`.
                    if (prop->constLocation && contains(*prop->constLocation))
                        return match("class_const", *prop->constLocation);
                }
                else if (auto method = member.get_if<Luau::AstClassMethod>())
                {
                    if (method->qualifierLocation && contains(*method->qualifierLocation))
                        return match(
                            method->visibility == Luau::AstClassMemberVisibility::Private ? "private" : "public", *method->qualifierLocation
                        );
                    if (contains(method->keywordLocation))
                        return match("function", method->keywordLocation);
                }
            }
        }
        else if (auto stat = (*it)->as<Luau::AstStatFunction>())
        {
            if (contains(stat->functionLocation))
                return match("function", stat->functionLocation);
        }
        else if (auto stat = (*it)->as<Luau::AstStatLocalFunction>())
        {
            // `const function f() ... end` is worth documenting as its own thing, distinct from a
            // plain `local function` or a local `const` -- it reads like neither on its own, and
            // the highlight should cover both keywords together, not just whichever one of the two
            // the cursor happened to land on.
            Luau::Location combined(stat->keywordLocation.begin, stat->functionLocation.end);
            if (contains(stat->keywordLocation))
                return stat->isConst ? match("const_function", combined) : match("local", stat->keywordLocation);
            if (contains(stat->functionLocation))
                return stat->isConst ? match("const_function", combined) : match("function", stat->functionLocation);
        }
    }

    Luau::AstNode* node = ancestry.back();

    if (auto stat = node->as<Luau::AstStatIf>())
    {
        if (contains(stat->ifLocation))
            // The same field marks both a leading `if` and a chained `elseif` -- tell them apart
            // by token length ("if" is 2 chars, "elseif" is 6), since both get their own doc.
            return stat->ifLocation.end.column - stat->ifLocation.begin.column == 2 ? match("if", stat->ifLocation)
                                                                                     : match("elseif", stat->ifLocation);
        if (stat->thenLocation && contains(*stat->thenLocation))
            return match("then", *stat->thenLocation);
        if (stat->elseLocation && contains(*stat->elseLocation))
            return match("else", *stat->elseLocation);
    }
    else if (auto expr = node->as<Luau::AstExprIfElse>())
    {
        // Documented separately from the statement: an `if` expression evaluates to a value, takes
        // exactly one expression per branch, and has no `end`. An `elseif` clause parses into a
        // nested AstExprIfElse whose own `ifLocation` is the `elseif` token, so tell the two apart
        // by token length the same way the statement case does.
        if (contains(expr->ifLocation))
            return expr->ifLocation.end.column - expr->ifLocation.begin.column == 2 ? match("if_expr", expr->ifLocation)
                                                                                    : match("elseif", expr->ifLocation);
        if (expr->thenLocation && contains(*expr->thenLocation))
            return match("then", *expr->thenLocation);
        if (expr->elseLocation && contains(*expr->elseLocation))
            return match("else", *expr->elseLocation);
    }
    else if (auto stat = node->as<Luau::AstStatWhile>())
    {
        if (contains(stat->whileLocation))
            return match("while", stat->whileLocation);
        if (stat->hasDo && contains(stat->doLocation))
            return match("do", stat->doLocation);
    }
    else if (auto stat = node->as<Luau::AstStatRepeat>())
    {
        if (contains(stat->repeatLocation))
            return match("repeat", stat->repeatLocation);
        if (contains(stat->untilLocation))
            return match("until", stat->untilLocation);
    }
    else if (auto stat = node->as<Luau::AstStatFor>())
    {
        if (contains(stat->forLocation))
            return match("for", stat->forLocation);
        if (stat->hasDo && contains(stat->doLocation))
            return match("do", stat->doLocation);
    }
    else if (auto stat = node->as<Luau::AstStatForIn>())
    {
        if (contains(stat->forLocation))
            return match("for", stat->forLocation);
        if (stat->hasIn && contains(stat->inLocation))
            return match("in", stat->inLocation);
        if (stat->hasDo && contains(stat->doLocation))
            return match("do", stat->doLocation);
    }
    else if (auto stat = node->as<Luau::AstStatReturn>())
    {
        if (contains(stat->returnLocation))
            return match("return", stat->returnLocation);
    }
    else if (node->is<Luau::AstStatBreak>())
    {
        return match("break", node->location);
    }
    else if (node->is<Luau::AstStatContinue>())
    {
        return match("continue", node->location);
    }
    else if (auto stat = node->as<Luau::AstStatLocal>())
    {
        if (stat->keywordLocation && contains(*stat->keywordLocation))
            return stat->isConst ? match("const", *stat->keywordLocation) : match("local", *stat->keywordLocation);
    }
    else if (auto stat = node->as<Luau::AstStatTypeAlias>())
    {
        if (stat->exported)
        {
            // Same trick as AstStatClass below: `exported` guarantees stat->location begins with
            // the literal 6-character `export` token (see luwu Parser.cpp's ident=="export"
            // handling), and there's no AstStatTypeAlias::exportLocation field to read instead.
            Luau::Location exportLocation(stat->location.begin, Luau::Position(stat->location.begin.line, stat->location.begin.column + 6));
            Luau::Location combined(exportLocation.begin, stat->typeLocation.end);
            if (contains(exportLocation) || contains(stat->typeLocation))
                return match("export_type", combined);
        }
        else if (contains(stat->typeLocation))
        {
            return match("type", stat->typeLocation);
        }
    }
    else if (auto stat = node->as<Luau::AstStatDeclareGlobal>())
    {
        if (contains(stat->declareLocation))
            return match("declare", stat->declareLocation);
    }
    else if (auto stat = node->as<Luau::AstStatDeclareFunction>())
    {
        if (contains(stat->declareLocation))
            return match("declare", stat->declareLocation);
        if (contains(stat->functionLocation))
            return match("function", stat->functionLocation);
    }
    else if (auto stat = node->as<Luau::AstStatDeclareExternType>())
    {
        if (contains(stat->declareLocation))
            return match("declare", stat->declareLocation);
        if (contains(stat->classLocation))
            return match("class", stat->classLocation);
        if (stat->extendsLocation && contains(*stat->extendsLocation))
            return match("extends", *stat->extendsLocation);
    }
    else if (auto stat = node->as<Luau::AstStatClass>())
    {
        if (stat->exported)
        {
            // `exported` guarantees stat->location begins with the literal 6-character `export`
            // token (see luwu Parser.cpp's parseClassStat/parseExportValue) -- there's no
            // AstStatClass::exportLocation field, so this is the only way to recover its span.
            Luau::Location exportLocation(stat->location.begin, Luau::Position(stat->location.begin.line, stat->location.begin.column + 6));
            Luau::Location combined(exportLocation.begin, stat->keywordLocation.end);
            if (contains(exportLocation) || contains(stat->keywordLocation))
                return match("export_class", combined);
        }
        else if (contains(stat->keywordLocation))
        {
            return match("class", stat->keywordLocation);
        }
    }
    else if (node->is<Luau::AstExprConstantNil>())
    {
        return match("nil", node->location);
    }
    else if (auto boolExpr = node->as<Luau::AstExprConstantBool>())
    {
        return match(boolExpr->value ? "true" : "false", node->location);
    }

    return std::nullopt;
}

std::optional<std::string> commentDirectiveDocKey(const Luau::HotComment& hotcomment)
{
    // `content` is everything after the `!`, with trailing space trimmed: "strict", "optimize 2",
    // "nolint LocalShadow". The directive is its first word; an argument belongs to the directive
    // rather than standing on its own.
    size_t nameEnd = hotcomment.content.find_first_of(" \t");
    std::string name = hotcomment.content.substr(0, nameEnd);

    if (name.empty())
        return std::nullopt;

    std::string docKey = "directive_" + name;

    // Documentation is what makes a directive worth pointing at, so a directive we cannot explain is
    // left looking like the ordinary comment it behaves as.
    if (!getKeywordHoverDocs(docKey))
        return std::nullopt;

    return docKey;
}

std::optional<KeywordHoverMatch> findCommentDirectiveDocKeyAtPosition(const std::vector<Luau::HotComment>& hotcomments, Luau::Position position)
{
    for (const Luau::HotComment& hc : hotcomments)
        if (hc.location.containsClosed(position))
            if (auto docKey = commentDirectiveDocKey(hc))
                return KeywordHoverMatch{std::move(*docKey), hc.location};

    return std::nullopt;
}
