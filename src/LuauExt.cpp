#include <optional>
#include <utility>

#include "Luau/Type.h"
#include "Platform/LSPPlatform.hpp"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/ToString.h"
#include "Luau/PrettyPrinter.h"
#include "Luau/TypeInfer.h"
#include "LSP/LuauExt.hpp"
#include "LSP/Utils.hpp"

namespace types
{
std::optional<std::string> getTypeName(Luau::TypeId typeId)
{
    std::optional<std::string> name = std::nullopt;
    auto ty = Luau::follow(typeId);

    if (auto typeName = Luau::getName(ty))
    {
        name = *typeName;
    }
    else if (auto mtv = Luau::get<Luau::MetatableType>(ty))
    {
        if (auto mtvName = Luau::getName(mtv->metatable))
            name = *mtvName;
    }
    else if (auto parentClass = Luau::get<Luau::ExternType>(ty))
    {
        name = parentClass->name;
    }
    // if (auto parentUnion = Luau::get<UnionType>(ty))
    // {
    //     return returnFirstNonnullOptionOfType<ClassType>(parentUnion);
    // }

    // strip synthetic typeof() for builtin tables
    if (name && name->compare(0, 7, "typeof(") == 0 && name->back() == ')')
        return name->substr(7, name->length() - 8);
    else
        return name;
}

std::optional<nlohmann::json> parseDefinitionsFileMetadata(const std::string& definitions)
{
    auto firstLine = getFirstLine(definitions);
    if (Luau::startsWith(firstLine, "--#METADATA#"))
    {
        firstLine = firstLine.substr(12);
        return json::parse(firstLine);
    }
    return std::nullopt;
}

Luau::LoadDefinitionFileResult registerDefinitions(
    Luau::Frontend& frontend, Luau::GlobalTypes& globals, const std::string& packageName, const std::string& definitions)
{
    return frontend.loadDefinitionFile(globals, globals.globalScope, definitions, packageName, /* captureComments = */ true);
}

using NameOrExpr = std::variant<std::string, Luau::AstExpr*>;

namespace
{
// How wide a printed type may be before it's broken across lines -- a hover box is narrower than an
// editor, and a line past this is read by scrolling sideways or not at all
static constexpr size_t kMaxLineWidth = 80;

// Finds the byte range of the first top-level "(...)" span in `s` (e.g. the parameter list of
// "function foo(a: number, b: string): boolean"), tracking paren depth so nested parens (in
// argument types) don't confuse the match. Returns [openParenIndex, onePastCloseParenIndex).
std::optional<std::pair<size_t, size_t>> findTopLevelParens(const std::string& s)
{
    auto openParen = s.find('(');
    if (openParen == std::string::npos)
        return std::nullopt;

    int depth = 0;
    for (size_t i = openParen; i < s.size(); i++)
    {
        if (s[i] == '(')
            depth++;
        else if (s[i] == ')')
        {
            depth--;
            if (depth == 0)
                return std::make_pair(openParen, i + 1);
        }
    }
    return std::nullopt;
}

// Splits a parameter list's inner content on top-level commas, skipping commas nested inside (),
// {}, [], or <> -- e.g. generic type arguments, table types, or tuple return types.
std::vector<std::string> splitTopLevelParams(const std::string& s)
{
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '(' || c == '{' || c == '[' || c == '<')
            depth++;
        else if (c == ')' || c == '}' || c == ']' || c == '>')
            depth--;
        else if (c == ',' && depth == 0)
        {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(s.substr(start));
    return parts;
}

// If printing `functionString` on the line it's given (at `baseIndent`) would run past
// `kMaxLineWidth`, reformats its parameter list Rust-style with one parameter per line, indented
// one level deeper than `baseIndent`, with the closing "): ReturnType" brought back to
// `baseIndent` -- so a long signature doesn't run off the edge of the hover box. Triggers on line
// length rather than parameter count, since a handful of long type names can overflow just as
// easily as many short ones.
std::string formatLongParamList(const std::string& functionString, const std::string& baseIndent)
{

    if (baseIndent.size() + functionString.size() <= kMaxLineWidth)
        return functionString;

    auto parens = findTopLevelParens(functionString);
    if (!parens)
        return functionString;
    auto [openParen, closeParenEnd] = *parens;

    std::string inner = functionString.substr(openParen + 1, closeParenEnd - openParen - 2);
    if (inner.empty())
        return functionString;

    auto params = splitTopLevelParams(inner);

    std::string result = functionString.substr(0, openParen + 1);
    for (size_t i = 0; i < params.size(); ++i)
    {
        std::string param = params[i];
        trim(param);
        result += "\n" + baseIndent + "    " + param + (i + 1 == params.size() ? "" : ",");
    }
    result += "\n" + baseIndent + functionString.substr(closeParenEnd - 1);
    return result;
}
// Luau prints an array or table type tight against its braces (`{string}`), which is denser than
// how the same type is written by hand; give the braces breathing room. String singletons are
// stepped over so a `"{"` inside one is left alone, and `{}` stays as it is.
std::string spaceOutBraces(const std::string& line)
{
    std::string result;
    char stringDelimiter = '\0';

    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];

        if (stringDelimiter != '\0')
        {
            result += c;
            if (c == '\\' && i + 1 < line.size())
                result += line[++i];
            else if (c == stringDelimiter)
                stringDelimiter = '\0';
            continue;
        }

        if (c == '"' || c == '\'')
        {
            stringDelimiter = c;
            result += c;
        }
        else if (c == '{' && i + 1 < line.size() && line[i + 1] != '}' && line[i + 1] != ' ')
        {
            result += "{ ";
        }
        else if (c == '}' && !result.empty() && result.back() != '{' && result.back() != ' ')
        {
            result += " }";
        }
        else
        {
            result += c;
        }
    }

    return result;
}

// Finds the first `open`..`close` group in `s` that holds more than one top-level element -- the
// one worth breaking across lines. A group holding a single element (an array of one table type,
// say) is skipped in favour of looking inside it, so `{{ a: X, b: Y }}` breaks on the struct rather
// than on the array wrapping it.
std::optional<std::pair<size_t, size_t>> findWrappableGroup(const std::string& s, char open, char close, size_t from = 0)
{
    for (size_t i = from; i < s.size(); ++i)
    {
        if (s[i] != open)
            continue;

        int depth = 0;
        for (size_t j = i; j < s.size(); ++j)
        {
            if (s[j] == open)
                depth++;
            else if (s[j] == close)
            {
                depth--;
                if (depth != 0)
                    continue;

                std::string inner = s.substr(i + 1, j - i - 1);
                if (splitTopLevelParams(inner).size() > 1)
                    return std::make_pair(i, j + 1);

                // Nothing to break here; the interesting group may still be nested inside
                if (auto nested = findWrappableGroup(s, open, close, i + 1); nested && nested->second <= j + 1)
                    return nested;

                i = j;
                break;
            }
        }
    }

    return std::nullopt;
}

// Breaks one over-long line at `group`, putting each of the group's elements on its own line and
// bringing the closing bracket back to the line's own indentation.
std::string breakGroupAcrossLines(const std::string& line, const std::string& indent, std::pair<size_t, size_t> group)
{
    auto [start, end] = group;

    auto elements = splitTopLevelParams(line.substr(start + 1, end - start - 2));

    std::string result = line.substr(0, start + 1);
    for (size_t i = 0; i < elements.size(); ++i)
    {
        std::string element = elements[i];
        trim(element);
        result += "\n" + indent + "    " + element + (i + 1 == elements.size() ? "" : ",");
    }
    result += "\n" + indent + line.substr(end - 1);
    return result;
}
} // namespace

namespace
{
// One line of a printed type, broken up if it runs long: a parameter list (a function signature, a
// class's primary constructor) first, otherwise a table type -- an array of struct-like tables is
// unreadable on one line. Runs over what it produces, so a long entry inside a broken-up group gets
// the same treatment.
std::string wrapLongLine(const std::string& line, size_t depth = 0)
{
    static constexpr size_t kMaxWrapDepth = 3;

    if (depth >= kMaxWrapDepth)
        return line;

    // An array of struct-like tables is unreadable on one line however short it happens to be, so
    // it breaks up on shape rather than on width
    const bool arrayOfStructs = [&]
    {
        auto array = findWrappableGroup(line, '{', '}');
        return array && line.compare(array->first, 2, "{ ") == 0 && line[array->first + 2] == '{';
    }();

    if (line.size() <= kMaxLineWidth && !arrayOfStructs)
        return line;

    size_t indentLength = 0;
    while (indentLength < line.size() && (line[indentLength] == ' ' || line[indentLength] == '\t'))
        ++indentLength;
    std::string indent = line.substr(0, indentLength);

    std::string wrapped;
    if (auto parens = findWrappableGroup(line, '(', ')'))
        wrapped = breakGroupAcrossLines(line, indent, *parens);
    else if (auto table = findWrappableGroup(line, '{', '}'))
        wrapped = breakGroupAcrossLines(line, indent, *table);
    else
        return line;

    std::string result;
    size_t lineStart = 0;
    while (lineStart <= wrapped.size())
    {
        size_t lineEnd = wrapped.find('\n', lineStart);
        std::string piece = wrapped.substr(lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);

        result += wrapLongLine(piece, depth + 1);

        if (lineEnd == std::string::npos)
            break;

        result += '\n';
        lineStart = lineEnd + 1;
    }

    return result;
}
} // namespace

std::string formatLongFunctionTypeLines(const std::string& typeString)
{
    std::string result;
    size_t lineStart = 0;
    // The indentation of the last line that began something, so a union broken across lines can be
    // laid out underneath it
    std::string enclosingIndent;

    while (lineStart <= typeString.size())
    {
        size_t lineEnd = typeString.find('\n', lineStart);
        std::string line = typeString.substr(lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);


        // Luau prints a long union or intersection one option per line, but flush against the left
        // margin and with a trailing space left behind on the line that introduced it -- which reads
        // as broken text (and leaves editors unable to highlight it). Lay the options out under the
        // line that introduced them instead.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

        size_t indentLength = 0;
        while (indentLength < line.size() && (line[indentLength] == ' ' || line[indentLength] == '\t'))
            ++indentLength;

        const bool isContinuation = indentLength < line.size() && (line[indentLength] == '|' || line[indentLength] == '&');
        if (isContinuation && indentLength == 0)
        {
            line = enclosingIndent + "    " + line;
            indentLength = enclosingIndent.size() + 4;
        }
        else if (!isContinuation)
        {
            enclosingIndent = line.substr(0, indentLength);
        }

        result += wrapLongLine(spaceOutBraces(line));

        if (lineEnd == std::string::npos)
            break;

        result += '\n';
        lineStart = lineEnd + 1;
    }

    return result;
}

// Converts an FTV and function call to a nice string
// In the format "function NAME(args): ret"
std::string toStringNamedFunction(const Luau::ModulePtr& module, const Luau::FunctionType* ftv, const NameOrExpr nameOrFuncExpr,
    std::optional<Luau::ScopePtr> scope, const ToStringNamedFunctionOpts& stringOpts)
{
    Luau::ToStringOptions opts;
    opts.functionTypeArguments = true;
    opts.hideNamedFunctionTypeParameters = false;
    opts.hideTableKind = stringOpts.hideTableKind;
    opts.useLineBreaks = stringOpts.multiline;
    opts.hideFunctionSelfArgument = stringOpts.hideFirstParameter;
    opts.hideFunctionSelfArgumentType = stringOpts.hideFirstParameterType;
    if (scope)
        opts.scope = *scope;
    auto functionString = Luau::toStringNamedFunction("", *ftv, opts);

    // HACK: remove all instances of "_: " from the function string
    // They don't look great, maybe we should upstream this as an option?
    replaceAll(functionString, "_: ", "");

    // Rust-style long-parameter-list wrapping only kicks in when the caller hasn't already
    // requested Luau's own multiline pretty-printing above (via `useLineBreaks`), to avoid the two
    // formatting styles fighting each other.
    auto finalize = [&](std::string result) -> std::string
    {
        if (stringOpts.multiline)
            return result;
        return formatLongParamList(result, stringOpts.baseIndent);
    };

    // If a name has already been provided, just use that
    if (auto name = std::get_if<std::string>(&nameOrFuncExpr))
    {
        return finalize("function " + *name + functionString);
    }
    auto funcExprPtr = std::get_if<Luau::AstExpr*>(&nameOrFuncExpr);

    // TODO: error here?
    if (!funcExprPtr)
        return finalize("function" + functionString);
    auto funcExpr = *funcExprPtr;

    // See if it's just in the form `func(args)`
    if (auto local = funcExpr->as<Luau::AstExprLocal>())
    {
        return finalize("function " + std::string(local->local->name.value) + functionString);
    }
    else if (auto global = funcExpr->as<Luau::AstExprGlobal>())
    {
        return finalize("function " + std::string(global->name.value) + functionString);
    }
    else if (funcExpr->as<Luau::AstExprGroup>() || funcExpr->as<Luau::AstExprFunction>())
    {
        // In the form (expr)(args), which implies that it's probably a IIFE
        return finalize("function" + functionString);
    }

    // See if the name belongs to a ClassType
    Luau::TypeId* parentIt = nullptr;
    std::string methodName;
    std::string baseName;

    if (auto indexName = funcExpr->as<Luau::AstExprIndexName>())
    {
        parentIt = module->astTypes.find(indexName->expr);
        methodName = std::string(1, indexName->op) + indexName->index.value;
        // If we are calling this as a method ':', we should implicitly hide self, and recompute the functionString
        opts.hideFunctionSelfArgument = indexName->op == ':';
        functionString = Luau::toStringNamedFunction("", *ftv, opts);
        replaceAll(functionString, "_: ", "");
        // We can try and give a temporary base name from what we can infer by the index, and then attempt to improve it with proper information
        baseName = Luau::toString(indexName->expr);
        trim(baseName); // Trim it, because toString is probably not meant to be used in this context (it has whitespace)
    }
    else if (auto indexExpr = funcExpr->as<Luau::AstExprIndexExpr>())
    {
        parentIt = module->astTypes.find(indexExpr->expr);
        methodName = "[" + Luau::toString(indexExpr->index) + "]";
        // We can try and give a temporary base name from what we can infer by the index, and then attempt to improve it with proper information
        baseName = Luau::toString(indexExpr->expr);
        // Trim it, because toString is probably not meant to be used in this context (it has whitespace)
        trim(baseName);
    }

    if (!parentIt)
        return finalize("function" + methodName + functionString);

    if (auto name = getTypeName(*parentIt))
        baseName = *name;

    return finalize("function " + baseName + methodName + functionString);
}

std::string toStringReturnType(Luau::TypePackId retTypes, Luau::ToStringOptions options)
{
    return toStringReturnTypeDetailed(retTypes, std::move(options)).name;
}

Luau::ToStringResult toStringReturnTypeDetailed(Luau::TypePackId retTypes, Luau::ToStringOptions options)
{
    size_t retSize = Luau::size(retTypes);
    bool hasTail = !Luau::finite(retTypes);
    bool wrap = Luau::get<Luau::TypePack>(Luau::follow(retTypes)) && (hasTail ? retSize != 0 : retSize > 1);

    auto result = Luau::toStringDetailed(retTypes, options);
    if (wrap)
        result.name = "(" + result.name + ")";
    return result;
}

// Duplicated from Luau/TypeInfer.h, since its static
std::optional<Luau::AstExpr*> matchRequire(const Luau::AstExprCall& call)
{
    const char* require = "require";

    if (call.args.size != 1)
        return std::nullopt;

    const Luau::AstExprGlobal* funcAsGlobal = call.func->as<Luau::AstExprGlobal>();
    if (!funcAsGlobal || funcAsGlobal->name != require)
        return std::nullopt;

    if (call.args.size != 1)
        return std::nullopt;

    return call.args.data[0];
}

std::optional<lsp::Location> getTypeLocation(Luau::TypeId ty, WorkspaceFileResolver* fileResolver)
{
    ty = Luau::follow(ty);

    auto moduleName = Luau::getDefinitionModuleName(ty);
    auto location = getLocation(ty);

    if (!moduleName || !location)
        return std::nullopt;

    auto document = fileResolver->getOrCreateTextDocumentFromModuleName(*moduleName);
    if (!document)
        return std::nullopt;

    return lsp::Location{
        document->uri(),
        lsp::Range{
            document->convertPosition(location->begin),
            document->convertPosition(location->end)}};
}

} // namespace types

struct FindNodeType : public Luau::AstVisitor
{
    Luau::Position pos;
    Luau::Position documentEnd;
    Luau::AstNode* best = nullptr;
    bool closed = false;

    explicit FindNodeType(Luau::Position pos, Luau::Position documentEnd, bool closed)
        : pos(pos)
        , documentEnd(documentEnd)
        , closed(closed)
    {
    }

    bool isCloserMatch(Luau::Location& newLocation) const
    {
        return (closed ? newLocation.containsClosed(pos) : newLocation.contains(pos)) && (!best || best->location.encloses(newLocation));
    }

    bool visit(Luau::AstNode* node) override
    {
        if (isCloserMatch(node->location))
        {
            best = node;
            return true;
        }

        // Edge case: If we ask for the node at the position that is the very end of the document
        // return the innermost AST element that ends at that position.

        if (node->location.end == documentEnd && pos >= documentEnd)
        {
            best = node;
            return true;
        }

        return false;
    }

    bool visit(class Luau::AstType* node) override
    {
        return visit(static_cast<Luau::AstNode*>(node));
    }

    bool visit(class Luau::AstTypePack* node) override
    {
        return visit(static_cast<Luau::AstNode*>(node));
    }

    bool visit(Luau::AstGenericType* node) override
    {
        return false;
    }

    bool visit(Luau::AstGenericTypePack* node) override
    {
        return false;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        visit(static_cast<Luau::AstNode*>(block));

        for (Luau::AstStat* stat : block->body)
        {
            if (stat->location.end < pos)
                continue;
            if (stat->location.begin > pos)
                break;

            stat->visit(this);
        }

        return false;
    }
};

Luau::AstNode* findNodeOrTypeAtPosition(const Luau::SourceModule& source, Luau::Position pos)
{
    const Luau::Position end = source.root->location.end;
    if (pos < source.root->location.begin)
        return source.root;

    if (pos > end)
        pos = end;

    FindNodeType findNode{pos, end, /* closed: */ false};
    findNode.visit(source.root);
    return findNode.best;
}

Luau::AstNode* findNodeOrTypeAtPositionClosed(const Luau::SourceModule& source, Luau::Position pos)
{
    const Luau::Position end = source.root->location.end;
    if (pos < source.root->location.begin)
        return source.root;

    if (pos > end)
        pos = end;

    FindNodeType findNode{pos, end, /* closed: */ true};
    findNode.visit(source.root);
    return findNode.best;
}

std::optional<Luau::Location> lookupTypeLocation(const Luau::Scope& deepScope, const Luau::Name& name)
{
    const Luau::Scope* scope = &deepScope;
    while (true)
    {
        auto it = scope->typeAliasLocations.find(name);
        if (it != scope->typeAliasLocations.end())
            return it->second;

        if (scope->parent)
            scope = scope->parent.get();
        else
            return std::nullopt;
    }
}

// Returns [base, property] - base is important during intersections
static std::vector<PropLookup> lookupProp(const Luau::TypeId& parentType, const Luau::Name& name, Luau::DenseHashSet<Luau::TypeId>& seenSet)
{
    if (seenSet.contains(parentType))
        return {};
    seenSet.insert(parentType);

    if (auto ctv = Luau::get<Luau::ExternType>(parentType))
    {
        if (auto prop = Luau::lookupExternTypeProp(ctv, name))
            return {PropLookup{parentType, *prop}};
    }
    else if (auto tbl = Luau::get<Luau::TableType>(parentType))
    {
        if (tbl->props.find(name) != tbl->props.end())
        {
            return {PropLookup{parentType, tbl->props.at(name)}};
        }
    }
    else if (auto mt = Luau::get<Luau::MetatableType>(parentType))
    {
        // Check the base table first
        auto baseTableTy = Luau::follow(mt->table);
        if (auto mtBaseTable = Luau::get<Luau::TableType>(baseTableTy))
        {
            if (mtBaseTable->props.find(name) != mtBaseTable->props.end())
            {
                return {PropLookup{baseTableTy, mtBaseTable->props.at(name)}};
            }
        }

        // If not found in base table, check __index in the metatable
        if (auto mtable = Luau::get<Luau::TableType>(Luau::follow(mt->metatable)))
        {
            auto indexIt = mtable->props.find("__index");
            if (indexIt != mtable->props.end() && indexIt->second.readTy)
            {
                Luau::TypeId followed = Luau::follow(*indexIt->second.readTy);
                if ((Luau::get<Luau::TableType>(followed) || Luau::get<Luau::MetatableType>(followed)) && followed != parentType) // ensure acyclic
                {
                    return lookupProp(followed, name, seenSet);
                }
                else if (Luau::get<Luau::FunctionType>(followed))
                {
                    // TODO: can we handle an index function...?
                    return {};
                }
            }
        }
    }
    else if (auto i = Luau::get<Luau::IntersectionType>(parentType))
    {
        for (Luau::TypeId ty : i->parts)
        {
            if (auto prop = lookupProp(Luau::follow(ty), name, seenSet); !prop.empty())
                return prop;
        }
    }
    else if (auto u = Luau::get<Luau::UnionType>(parentType))
    {
        std::vector<PropLookup> options;
        for (Luau::TypeId ty : u->options)
        {
            if (auto prop = lookupProp(Luau::follow(ty), name, seenSet); !prop.empty())
                options.insert(options.end(), prop.begin(), prop.end());
        }

        return options;
    }
    return {};
}

std::vector<PropLookup> lookupProp(const Luau::TypeId& parentType, const Luau::Name& name)
{
    Luau::DenseHashSet<Luau::TypeId> seenSet{nullptr};
    return lookupProp(parentType, name, seenSet);
}

std::optional<Luau::ModuleName> lookupImportedModule(const Luau::Scope& deepScope, const Luau::Name& name)
{
    const Luau::Scope* scope = &deepScope;
    while (true)
    {
        auto it = scope->importedModules.find(name);
        if (it != scope->importedModules.end())
            return it->second;

        if (scope->parent)
            scope = scope->parent.get();
        else
            return std::nullopt;
    }
}

bool types::isMetamethod(const Luau::Name& name)
{
    return name == "__index" || name == "__newindex" || name == "__call" || name == "__concat" || name == "__unm" || name == "__add" ||
           name == "__sub" || name == "__mul" || name == "__div" || name == "__mod" || name == "__pow" || name == "__tostring" ||
           name == "__metatable" || name == "__eq" || name == "__lt" || name == "__le" || name == "__mode" || name == "__iter" || name == "__len";
}

lsp::Position toUTF16(const TextDocument* textDocument, const Luau::Position& position)
{
    if (textDocument)
        return textDocument->convertPosition(position);
    else
        return lsp::Position{static_cast<size_t>(position.line), static_cast<size_t>(position.column)};
}

lsp::Diagnostic createTypeErrorDiagnostic(const Luau::TypeError& error, Luau::FileResolver* fileResolver, const TextDocument* textDocument)
{
    std::string message;
    if (const auto* syntaxError = Luau::get_if<Luau::SyntaxError>(&error.data))
    {
        message = "SyntaxError: " + syntaxError->message;

        // The LSP offers a quick fix for this specific error (see generateClassMemberPublicFix in
        // CodeAction.cpp) -- hint at it here, since this is LSP-specific UX and doesn't belong in
        // the parser's own error message.
        if (syntaxError->message.find("Class contains a 'private' member") != std::string::npos)
            message += " (quick fix available)";
    }
    else
        message = "TypeError: " + Luau::toString(error, Luau::TypeErrorToStringOptions{fileResolver});

    lsp::Diagnostic diagnostic;
    diagnostic.source = "Luwu";
    diagnostic.code = error.code();
    diagnostic.message = message;
    diagnostic.severity = lsp::DiagnosticSeverity::Error;
    diagnostic.range = {toUTF16(textDocument, error.location.begin), toUTF16(textDocument, error.location.end)};
    diagnostic.codeDescription = {Uri::parse("https://luau.org/types")};
    return diagnostic;
}

lsp::Diagnostic createLintDiagnostic(const Luau::LintWarning& lint, const TextDocument* textDocument)
{
    std::string lintName = Luau::LintWarning::getName(lint.code);

    lsp::Diagnostic diagnostic;
    diagnostic.source = "Luwu";
    diagnostic.code = lint.code;
    diagnostic.message = lintName + ": " + lint.text;
    diagnostic.severity = lsp::DiagnosticSeverity::Warning; // Configuration can convert this to an error
    diagnostic.range = {toUTF16(textDocument, lint.location.begin), toUTF16(textDocument, lint.location.end)};
    diagnostic.codeDescription = {Uri::parse("https://luau.org/lint#" + toLower(lintName) + "-" + std::to_string(static_cast<int>(lint.code)))};

    if (lint.code == Luau::LintWarning::Code::Code_LocalUnused || lint.code == Luau::LintWarning::Code::Code_ImportUnused ||
        lint.code == Luau::LintWarning::Code::Code_FunctionUnused)
    {
        diagnostic.tags.emplace_back(lsp::DiagnosticTag::Unnecessary);
    }
    else if (lint.code == Luau::LintWarning::Code::Code_DeprecatedApi || lint.code == Luau::LintWarning::Code::Code_DeprecatedGlobal)
    {
        diagnostic.tags.emplace_back(lsp::DiagnosticTag::Deprecated);
    }

    return diagnostic;
}

lsp::Diagnostic createParseErrorDiagnostic(const Luau::ParseError& error, const TextDocument* textDocument)
{
    lsp::Diagnostic diagnostic;
    diagnostic.source = "Luwu";
    diagnostic.code = "SyntaxError";
    diagnostic.message = "SyntaxError: " + error.getMessage();
    diagnostic.severity = lsp::DiagnosticSeverity::Error;
    diagnostic.range = {toUTF16(textDocument, error.getLocation().begin), toUTF16(textDocument, error.getLocation().end)};
    diagnostic.codeDescription = {Uri::parse("https://luau.org/syntax")};
    return diagnostic;
}

// Based on upstream, except we use containsClosed
struct FindExprOrLocalClosed : public Luau::AstVisitor
{
    Luau::Position pos;
    Luau::ExprOrLocal result;

    explicit FindExprOrLocalClosed(Luau::Position pos)
        : pos(pos)
    {
    }

    // We want to find the result with the smallest location range.
    bool isCloserMatch(Luau::Location newLocation)
    {
        auto current = result.getLocation();
        return newLocation.containsClosed(pos) && (!current || current->encloses(newLocation));
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* stat : block->body)
        {
            if (stat->location.end < pos)
                continue;
            if (stat->location.begin > pos)
                break;

            stat->visit(this);
        }

        return false;
    }

    bool visit(Luau::AstExpr* expr) override
    {
        if (isCloserMatch(expr->location))
        {
            result.setExpr(expr);
            return true;
        }
        return false;
    }

    bool visitLocal(Luau::AstLocal* local)
    {
        if (isCloserMatch(local->location))
        {
            result.setLocal(local);
            return true;
        }
        return false;
    }

    bool visit(Luau::AstStatLocalFunction* function) override
    {
        visitLocal(function->name);
        return true;
    }

    bool visit(Luau::AstStatLocal* al) override
    {
        for (size_t i = 0; i < al->vars.size; ++i)
        {
            visitLocal(al->vars.data[i]);
        }
        return true;
    }

    bool visit(Luau::AstExprFunction* fn) override
    {
        for (size_t i = 0; i < fn->args.size; ++i)
        {
            visitLocal(fn->args.data[i]);
        }
        return visit((Luau::AstExpr*)fn);
    }

    bool visit(Luau::AstStatFor* forStat) override
    {
        visitLocal(forStat->var);
        return true;
    }

    bool visit(Luau::AstStatForIn* forIn) override
    {
        for (Luau::AstLocal* var : forIn->vars)
        {
            visitLocal(var);
        }
        return true;
    }

    bool visit(Luau::AstStatClass* classStat) override
    {
        visitLocal(classStat->name);
        return true;
    }
};

Luau::ExprOrLocal findExprOrLocalAtPositionClosed(const Luau::SourceModule& source, Luau::Position pos)
{
    FindExprOrLocalClosed findVisitor{pos};
    findVisitor.visit(source.root);
    return findVisitor.result;
}

struct FindSymbolReferences : public Luau::AstVisitor
{
    Luau::Symbol symbol;
    std::vector<Luau::Location> result{};

    explicit FindSymbolReferences(Luau::Symbol symbol)
        : symbol(symbol)
    {
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* stat : block->body)
        {
            stat->visit(this);
        }

        return false;
    }

    bool visitLocal(Luau::AstLocal* local) const
    {
        return Luau::Symbol(local) == symbol;
    }

    bool visitGlobal(const Luau::AstName& name) const
    {
        return Luau::Symbol(name) == symbol;
    }

    bool visit(Luau::AstStatLocalFunction* function) override
    {
        if (visitLocal(function->name))
        {
            result.push_back(function->name->location);
        }
        return true;
    }

    bool visit(Luau::AstStatLocal* al) override
    {
        for (size_t i = 0; i < al->vars.size; ++i)
        {
            if (visitLocal(al->vars.data[i]))
            {
                result.push_back(al->vars.data[i]->location);
            }
        }
        return true;
    }

    bool visit(Luau::AstExprLocal* local) override
    {
        if (visitLocal(local->local))
        {
            result.push_back(local->location);
        }
        return true;
    }

    bool visit(Luau::AstExprGlobal* global) override
    {
        if (visitGlobal(global->name))
        {
            result.push_back(global->location);
        }
        return true;
    }

    bool visit(Luau::AstExprFunction* fn) override
    {
        for (size_t i = 0; i < fn->args.size; ++i)
        {
            if (visitLocal(fn->args.data[i]))
            {
                result.push_back(fn->args.data[i]->location);
            }
        }
        return true;
    }

    bool visit(Luau::AstStatFor* forStat) override
    {
        if (visitLocal(forStat->var))
        {
            result.push_back(forStat->var->location);
        }
        return true;
    }

    bool visit(Luau::AstStatForIn* forIn) override
    {
        for (auto var : forIn->vars)
        {
            if (visitLocal(var))
            {
                result.push_back(var->location);
            }
        }
        return true;
    }

    bool visit(Luau::AstType* type) override
    {
        return true;
    }

    bool visit(Luau::AstTypePack* type) override
    {
        return true;
    }

    bool visit(Luau::AstTypeReference* typeReference) override
    {
        // TODO: this is not *completely* correct in the case of shadowing, as it is just a name comparison
        // Upstream issue: https://github.com/luau-lang/luau/issues/1108
        if (typeReference->prefix && symbol.local && typeReference->prefix.value() == symbol.astName())
            result.push_back(typeReference->prefixLocation.value());
        return true;
    }

    bool visit(Luau::AstStatClass* classStat) override
    {
        if (visitLocal(classStat->name))
        {
            result.push_back(classStat->name->location);
        }
        return true;
    }
};

std::vector<Luau::Location> findSymbolReferences(const Luau::SourceModule& source, Luau::Symbol symbol)
{
    FindSymbolReferences finder(symbol);
    source.root->visit(&finder);
    return std::move(finder.result);
}

struct FindTypeReferences : public Luau::AstVisitor
{
    Luau::Name typeName;
    std::optional<const Luau::Name> prefix;
    std::vector<Luau::Location> result{};

    explicit FindTypeReferences(Luau::Name typeName, std::optional<const Luau::Name> prefix)
        : typeName(std::move(typeName))
        , prefix(std::move(prefix))
    {
    }

    bool visit(class Luau::AstType* node) override
    {
        return true;
    }

    bool visit(class Luau::AstTypePack* node) override
    {
        return true;
    }

    bool visit(class Luau::AstTypeReference* node) override
    {
        if (node->name.value == typeName && ((!prefix && !node->prefix) || (prefix && node->prefix && node->prefix->value == prefix.value())))
            result.push_back(node->nameLocation);

        return true;
    }
};

std::vector<Luau::Location> types::findClassNameReferences(const Luau::SourceModule& source, Luau::AstStatClass* classStat)
{
    std::vector<Luau::Location> result = findSymbolReferences(source, Luau::Symbol(classStat->name));

    // Class names aren't pushed as locals (that's what lets classes be hoisted), so every value usage
    // (`Dog(...)`, `Dog.staticFn`, passing `Dog` around) parses as a global. Classes can't be shadowed
    // by another class, and a local of the same name would parse as `AstExprLocal`, so any global with
    // the class's name refers to the class.
    auto valueReferences = findSymbolReferences(source, Luau::Symbol(classStat->name->name));
    result.insert(result.end(), valueReferences.begin(), valueReferences.end());

    auto typeReferences = findTypeReferences(source, classStat->name->name.value, std::nullopt);
    result.insert(result.end(), typeReferences.begin(), typeReferences.end());

    return result;
}

namespace
{
// An extern type produced by a class statement has its own `definitionLocation` set to that
// class statement's location. An "object" (instance) extern type instead points back to its
// class's extern type via its nominal `relation`, and vice versa - so we check one hop of that
// relation too, to treat `SomeClass.staticFn` and `someInstance:method()` uniformly.
std::optional<Luau::Location> externTypeDefinitionLocation(Luau::TypeId ty)
{
    ty = Luau::follow(ty);
    const auto* externType = Luau::get<Luau::ExternType>(ty);
    if (!externType)
        return std::nullopt;

    if (externType->definitionLocation)
        return externType->definitionLocation;

    if (externType->relation)
    {
        Luau::TypeId relatedTy;
        if (const auto* obj = externType->relation->get_if<Luau::Obj>())
            relatedTy = obj->ty;
        else if (const auto* klass = externType->relation->get_if<Luau::Klass>())
            relatedTy = klass->ty;
        else
            return std::nullopt;

        if (const auto* relatedExternType = Luau::get<Luau::ExternType>(Luau::follow(relatedTy)))
            return relatedExternType->definitionLocation;
    }

    return std::nullopt;
}
} // namespace

namespace types
{
namespace
{
struct FindEnclosingClassStat : Luau::AstVisitor
{
    Luau::Position targetPosition;
    Luau::AstStatClass* result = nullptr;

    explicit FindEnclosingClassStat(const Luau::Position& position)
        : targetPosition(position)
    {
    }

    bool visit(Luau::AstStatClass* node) override
    {
        if (node->location.containsClosed(targetPosition))
            result = node;
        return true;
    }
};
} // namespace

Luau::AstStatClass* findEnclosingClassStat(Luau::AstStatBlock* root, const Luau::Position& position)
{
    FindEnclosingClassStat finder(position);
    root->visit(&finder);
    return finder.result;
}
} // namespace types

Luau::AstStatClass* types::findClassStatFromExternType(Luau::AstStatBlock* root, Luau::TypeId ty)
{
    auto location = externTypeDefinitionLocation(ty);
    if (!location)
        return nullptr;

    for (Luau::AstStat* stat : root->body)
        if (auto* classStat = stat->as<Luau::AstStatClass>())
            if (classStat->location == *location)
                return classStat;

    return nullptr;
}

namespace
{
// Covers both fields and methods/static functions: usage sites (`obj.member`/`obj:member`) look
// identical at the AST level regardless of which kind of member is being accessed.
struct FindClassMemberUsages : public Luau::AstVisitor
{
    const Luau::ModulePtr& module;
    Luau::AstName memberName;
    Luau::Location classLocation;
    std::vector<Luau::Location> result;

    FindClassMemberUsages(const Luau::ModulePtr& module, Luau::AstName memberName, Luau::Location classLocation)
        : module(module)
        , memberName(memberName)
        , classLocation(classLocation)
    {
    }

    bool visit(Luau::AstExprIndexName* indexName) override
    {
        if (indexName->index == memberName)
        {
            if (auto ty = module->astTypes.find(indexName->expr))
            {
                if (auto location = externTypeDefinitionLocation(*ty); location && *location == classLocation)
                    result.push_back(indexName->indexLocation);
            }
        }

        return true;
    }

    bool visit(Luau::AstStatClass* classStat) override
    {
        if (classStat->location == classLocation)
        {
            for (const auto& member : classStat->members)
            {
                if (const auto* prop = member.get_if<Luau::AstClassProperty>())
                {
                    if (prop->name == memberName)
                        result.push_back(prop->nameLocation);
                }
                else if (const auto* method = member.get_if<Luau::AstClassMethod>())
                {
                    if (method->functionName == memberName)
                        result.push_back(method->nameLocation);
                }
            }
        }

        // Continue descending, since methods (including this one) may reference the target member
        // recursively via `self.member`/`self:member()` in their own bodies.
        return true;
    }
};
} // namespace

std::vector<Luau::Location> types::findClassMemberReferences(
    const Luau::SourceModule& source, const Luau::ModulePtr& module, Luau::AstStatClass* classStat, const Luau::AstName& methodName)
{
    FindClassMemberUsages finder(module, methodName, classStat->location);
    source.root->visit(&finder);
    return std::move(finder.result);
}

std::vector<Luau::Location> findTypeReferences(const Luau::SourceModule& source, const Luau::Name& typeName, std::optional<const Luau::Name> prefix)
{
    FindTypeReferences finder(typeName, std::move(prefix));
    source.root->visit(&finder);
    return std::move(finder.result);
}

std::optional<Luau::Location> getLocation(Luau::TypeId type)
{
    type = follow(type);

    if (auto ftv = Luau::get<Luau::FunctionType>(type))
    {
        if (ftv->definition)
            return ftv->definition->originalNameLocation;
    }
    else if (auto ttv = Luau::get<Luau::TableType>(type))
    {
        return ttv->definitionLocation;
    }
    else if (auto mtv = Luau::get<Luau::MetatableType>(type))
    {
        return getLocation(mtv->table);
    }
    else if (auto ctv = Luau::get<Luau::ExternType>(type))
    {
        return ctv->definitionLocation;
    }

    return std::nullopt;
}

bool isGetService(const Luau::AstExpr* expr)
{
    if (auto call = expr->as<Luau::AstExprCall>())
        if (auto index = call->func->as<Luau::AstExprIndexName>())
            if (index->index == "GetService")
                if (auto name = index->expr->as<Luau::AstExprGlobal>())
                    if (name->name == "game")
                        return true;

    return false;
}

bool isRequire(const Luau::AstExpr* expr)
{
    if (auto call = expr->as<Luau::AstExprCall>())
    {
        if (auto funcAsGlobal = call->func->as<Luau::AstExprGlobal>(); funcAsGlobal && funcAsGlobal->name == "require")
            return true;
    }
    else if (auto assertion = expr->as<Luau::AstExprTypeAssertion>())
    {
        return isRequire(assertion->expr);
    }

    return false;
}

bool isMethod(const Luau::FunctionType* ftv)
{
    if (ftv->hasSelf)
        return true;

    // TODO: hasSelf is not always specified, so we manually check for the "self" name (https://github.com/Roblox/luau/issues/551)
    if (ftv->argNames.size() > 0 && ftv->argNames[0].has_value() && ftv->argNames[0]->name == "self")
    {
        return true;
    }

    return false;
}

static bool isMethod(Luau::TypeId ty)
{
    auto ftv = Luau::get<Luau::FunctionType>(Luau::follow(ty));
    if (!ftv)
        return false;

    return isMethod(ftv);
}

bool isOverloadedMethod(Luau::TypeId ty)
{
    if (!Luau::get<Luau::IntersectionType>(Luau::follow(ty)))
        return false;

    auto isOverloadedMethod = [](Luau::TypeId part) -> bool
    {
        return isMethod(part);
    };

    std::vector<Luau::TypeId> parts = Luau::flattenIntersection(ty);
    return std::all_of(parts.begin(), parts.end(), isOverloadedMethod);
}

std::optional<Luau::TypeId> findCallMetamethod(Luau::TypeId type)
{
    type = Luau::follow(type);

    std::optional<Luau::TypeId> metatable;
    if (const auto mtType = Luau::get<Luau::MetatableType>(type))
        metatable = mtType->metatable;
    else if (const auto classType = Luau::get<Luau::ExternType>(type))
        metatable = classType->metatable;

    if (!metatable)
        return std::nullopt;

    auto unwrapped = Luau::follow(*metatable);
    if (auto prop = lookupProp(unwrapped, "__call"); prop.size() == 1 && prop[0].property.readTy)
    {
        return prop[0].property.readTy;
    }

    return std::nullopt;
}

namespace types
{
namespace
{
struct FindClassStatContainingPosition : Luau::AstVisitor
{
    Luau::Position targetPosition;
    Luau::AstStatClass* result = nullptr;

    explicit FindClassStatContainingPosition(const Luau::Position& position)
        : targetPosition(position)
    {
    }

    bool visit(Luau::AstStatClass* node) override
    {
        if (node->location.containsClosed(targetPosition))
        {
            // Only consider the position "in" this class if it isn't actually nested inside one of
            // its existing methods (e.g. in the parameter list, in the body, or on a trailing blank
            // line after the last statement but before that method's own `end`) -- sitting inside an
            // unrelated method shouldn't count as being directly in the class body.
            //
            // We use the method's full location (signature through its own `end`), not just the
            // body's, so this also covers the parameter list and any trailing blank lines the body
            // block's own location doesn't extend to (e.g. right after a table constructor that was
            // the last statement).
            //
            // We only trust this at all when the *class itself* closed cleanly (`node->hasEnd`).
            // When the user is mid-typing an incomplete member (e.g. `function _` with no `(`/`end`
            // yet), error recovery can attribute the class's own closing `end` to that broken member
            // instead, making it falsely look like a complete, "existing" method that contains the
            // cursor -- and simultaneously leaves the class's own `hasEnd` false, which is how we
            // detect and ignore that case.
            bool insideExistingMethod = false;
            if (node->hasEnd)
            {
                for (const auto& member : node->members)
                {
                    if (const auto* method = member.get_if<Luau::AstClassMethod>();
                        method && method->function && method->function->body && method->function->body->hasEnd &&
                        method->function->location.containsClosed(targetPosition))
                    {
                        insideExistingMethod = true;
                        break;
                    }
                }
            }

            if (!insideExistingMethod)
                result = node;
        }
        return true;
    }
};
} // namespace

Luau::AstStatClass* findClassStatContainingPosition(Luau::AstStatBlock* root, const Luau::Position& position)
{
    FindClassStatContainingPosition finder(position);
    root->visit(&finder);
    return finder.result;
}

} // namespace types
