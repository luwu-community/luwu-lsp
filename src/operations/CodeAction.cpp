#include "LSP/LanguageServer.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/Refactoring.hpp"
#include "Protocol/CodeAction.hpp"
#include "LSP/LuauExt.hpp"
#include "Luau/PrettyPrinter.h"
#include "Luau/LinterConfig.h"
#include "Platform/AutoImports.hpp"
#include "Platform/LSPPlatform.hpp"
#include "Luau/Ast.h"
#include "Luau/Error.h"
#include "LSP/Destructure.hpp"

#include <algorithm>
#include <unordered_set>

LUAU_FASTFLAG(LuauSolverV2)
LUAU_FASTFLAG(LuwuDoExpr)

namespace
{
using namespace Luau::LanguageServer::Destructure;

// Visitor to find the statement containing a local variable at a specific location
struct FindStatementContainingLocal : Luau::AstVisitor
{
    Luau::Location targetLocation;
    Luau::AstStat* result = nullptr;

    explicit FindStatementContainingLocal(const Luau::Location& location)
        : targetLocation(location)
    {
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
        {
            if (node->vars.data[i]->location == targetLocation)
            {
                result = node;
                return false;
            }
        }
        return true;
    }

    bool visit(Luau::AstStatLocalFunction* node) override
    {
        if (node->name->location == targetLocation)
        {
            result = node;
            return false;
        }
        return true;
    }
};

Luau::AstStat* findStatementContainingLocal(Luau::AstStatBlock* root, const Luau::Location& location)
{
    FindStatementContainingLocal finder(location);
    root->visit(&finder);
    return finder.result;
}

// The parameter list of whatever function (or class primary constructor) `position` sits in, as a
// source location covering the parentheses themselves.
struct FindParameterListAtPosition : Luau::AstVisitor
{
    Luau::Position targetPosition;
    std::optional<Luau::Location> result;

    explicit FindParameterListAtPosition(const Luau::Position& position)
        : targetPosition(position)
    {
    }

    bool visit(Luau::AstExprFunction* node) override
    {
        if (node->argLocation && node->argLocation->containsClosed(targetPosition))
            result = node->argLocation;
        return true;
    }

    bool visit(Luau::AstStatClass* node) override
    {
        if (node->primaryConstructor && node->primaryConstructor->argLocation.containsClosed(targetPosition))
            result = node->primaryConstructor->argLocation;
        return true;
    }
};

// A code action rewriting the layout of a comma-separated list. Offered first and as a quick fix when the client
// takes quick fixes, so it heads the menu whenever the cursor is in a list; a client asking only for refactorings
// gets it as a rewrite.
void addListLayoutAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, std::string title, const lsp::Range& range,
    std::string newText, std::vector<lsp::CodeAction>& result)
{
    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = context.wants(lsp::CodeActionKind::QuickFix) ? lsp::CodeActionKind::QuickFix : lsp::CodeActionKind::RefactorRewrite;

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::vector{lsp::TextEdit{range, std::move(newText)}});
    action.edit = workspaceEdit;

    result.push_back(action);
}

// `elements` one per line inside parentheses (Rust style, the closing paren back at the indentation of the line the
// list starts on), with no trailing comma
std::string layOutOnePerLine(const std::vector<std::string>& elements, const TextDocument& textDocument, size_t line)
{
    std::string baseIndent = getLineIndentation(textDocument, line);
    std::string elementIndent = baseIndent + detectIndentUnit(textDocument);

    std::string text = "(\n";
    for (size_t i = 0; i < elements.size(); ++i)
        text += elementIndent + elements[i] + (i + 1 == elements.size() ? "\n" : ",\n");
    text += baseIndent + ")";
    return text;
}

// Rewrites a parameter list between one-per-line and all-on-one-line. Offered on any function, method, or class or
// trait primary constructor.
void generateParameterListWrapAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, const Luau::Location& argLocation,
    const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    lsp::Range range = textDocument.convertLocation(argLocation);
    std::string text = textDocument.getText(range);
    if (text.size() < 2 || text.front() != '(' || text.back() != ')')
        return;

    auto parameters = splitParameters(text.substr(1, text.size() - 2));
    if (parameters.empty() || (parameters.size() == 1 && parameters[0].empty()))
        return;

    if (text.find('\n') != std::string::npos)
        addListLayoutAction(uri, context, "Put parameters on one line", range, "(" + layOutOnOneLine(parameters) + ")", result);
    else
        addListLayoutAction(
            uri, context, "Put each parameter on its own line", range, layOutOnePerLine(parameters, textDocument, argLocation.begin.line), result);
}

// Luwu Traits (rfcs/classes/traits.md): a class's `implements` list or a trait's `needs` list, as written in the
// source. The AST doesn't record the parentheses an `implements (A, B)` list may be wrapped in, so they're found here.
struct TraitList
{
    Luau::Location keyword;
    // From the first trait (or the opening paren) to the last trait (or the closing paren)
    Luau::Location list;
    // The traits themselves, without parentheses or surrounding whitespace
    Luau::Location contents;
};

std::optional<TraitList> findTraitList(
    const std::optional<Luau::Location>& keyword, const Luau::AstArray<Luau::AstClassTraitRef>& refs, const TextDocument& textDocument)
{
    if (!keyword || refs.size == 0)
        return std::nullopt;

    const std::string text = textDocument.getText();
    auto offsetOf = [&](const Luau::Position& position)
    {
        return textDocument.offsetAt(textDocument.convertPosition(position));
    };
    auto positionOf = [&](size_t offset)
    {
        return textDocument.convertPosition(textDocument.positionAt(offset));
    };

    TraitList traitList;
    traitList.keyword = *keyword;
    traitList.contents = Luau::Location{refs.data[0].location.begin, refs.data[refs.size - 1].location.end};
    traitList.list = traitList.contents;

    size_t open = skipWhitespaceAndComments(text, offsetOf(keyword->end));
    if (open < text.size() && text[open] == '(' && open < offsetOf(traitList.contents.begin))
    {
        size_t close = skipWhitespaceAndComments(text, offsetOf(traitList.contents.end));
        if (close >= text.size() || text[close] != ')')
            return std::nullopt;

        traitList.list = Luau::Location{positionOf(open), positionOf(close + 1)};
    }

    return traitList;
}

// Rewrites an `implements` or `needs` list between one trait per line, in parentheses, and all on one line without
// them.
void generateTraitListWrapAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, const TraitList& traitList,
    const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    lsp::Range range = textDocument.convertLocation(traitList.list);
    auto traits = splitParameters(textDocument.getText(textDocument.convertLocation(traitList.contents)));

    if (textDocument.getText(range).find('\n') != std::string::npos)
        addListLayoutAction(uri, context, "Put traits on one line", range, layOutOnOneLine(traits), result);
    else
    {
        // Only the list is replaced, so `implements A, B` keeps its space and becomes `implements (`
        std::string newText = layOutOnePerLine(traits, textDocument, traitList.keyword.begin.line);
        addListLayoutAction(uri, context, "Put each trait on its own line", range, newText, result);
    }
}

// The `implements` or `needs` list `position` sits in, keyword included
struct FindTraitListAtPosition : Luau::AstVisitor
{
    Luau::Position targetPosition;
    const TextDocument& textDocument;
    std::optional<TraitList> result;

    FindTraitListAtPosition(const Luau::Position& position, const TextDocument& textDocument)
        : targetPosition(position)
        , textDocument(textDocument)
    {
    }

    bool visit(Luau::AstStatClass* node) override
    {
        if (!node->location.containsClosed(targetPosition))
            return false;

        for (auto traitList : {findTraitList(node->implementsLocation, node->implements, textDocument),
                 findTraitList(node->needsLocation, node->needs, textDocument)})
        {
            if (traitList && Luau::Location{traitList->keyword.begin, traitList->list.end}.containsClosed(targetPosition))
                result = traitList;
        }
        return true;
    }
};

// Luwu Destructuring (rfcs/destructuring.md): the innermost `.{...}` pattern `position` sits in, so a cursor in
// `components.{ Card, Menu }` lays out that pattern rather than the one around it. `pattern` is unset for a
// `type .{A, B} = m`, which has no nested patterns.
struct DestructurePatternAt
{
    Luau::Location location;
    const Luau::AstDestructurePattern* pattern = nullptr;
};

struct FindDestructurePatternAtPosition : Luau::AstVisitor
{
    Luau::Position targetPosition;
    std::optional<DestructurePatternAt> result;

    explicit FindDestructurePatternAtPosition(const Luau::Position& position)
        : targetPosition(position)
    {
    }

    void consider(const Luau::AstDestructurePattern& pattern)
    {
        if (!pattern.location || !pattern.closed || !pattern.location->containsClosed(targetPosition))
            return;

        result = DestructurePatternAt{*pattern.location, &pattern};
        for (const auto& field : pattern.fields)
            consider(field.target);
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        if (node->destructure)
            consider(*node->destructure);
        return true;
    }

    bool visit(Luau::AstStatForIn* node) override
    {
        for (const auto* pattern : node->destructure)
            if (pattern)
                consider(*pattern);
        return true;
    }

    bool visit(Luau::AstStatTypeAlias* node) override
    {
        if (node->destructure && node->destructure->closed && node->destructure->location.containsClosed(targetPosition))
            result = DestructurePatternAt{node->destructure->location};
        return true;
    }
};

// Rewrites a destructuring pattern between one field per line and all on one line, like a parameter list
void generateDestructureLayoutAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, const DestructurePatternAt& at,
    const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    lsp::Range range = textDocument.convertLocation(at.location);

    if (at.location.begin.line != at.location.end.line)
    {
        if (auto text = layOutDestructureOnOneLine(at.location, at.pattern, textDocument))
            addListLayoutAction(uri, context, "Put fields on one line", range, *text, result);
    }
    else if (auto text = layOutDestructureOnePerLine(at.location, textDocument))
        addListLayoutAction(uri, context, "Put each field on its own line", range, *text, result);
}

void addEditsAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, std::string title, std::vector<lsp::TextEdit> edits,
    std::vector<lsp::CodeAction>& result)
{
    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = context.wants(lsp::CodeActionKind::QuickFix) ? lsp::CodeActionKind::QuickFix : lsp::CodeActionKind::RefactorRewrite;

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));
    action.edit = workspaceEdit;

    result.push_back(action);
}

// `components.Card` under the cursor: add `Card` to the pattern that binds `components` (or give `components` one),
// and turn every `components.Card` in the file into `Card`
void generateImportFieldAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, const Luau::Position& position,
    const Luau::SourceModule& sourceModule, const Luau::Module& module, Luau::NotNull<Luau::BuiltinTypes> builtinTypes,
    const DestructureFacts& facts, const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    Luau::AstLocal* qualifier = nullptr;
    std::string name;
    for (auto* index : facts.indexes)
        if (auto* object = index->expr->as<Luau::AstExprLocal>(); object && index->op == '.' && index->location.containsClosed(position))
            qualifier = object->local, name = index->index.value;
    for (auto* ref : facts.typeRefs)
        if (ref->prefix && ref->prefixLocal && ref->location.containsClosed(position))
            qualifier = ref->prefixLocal, name = ref->name.value;
    if (!qualifier || isHiddenDestructureLocal(qualifier))
        return;

    bool withType = bringsAType(fieldType(typeOfLocal(qualifier, module, facts), name), builtinTypes);

    std::optional<lsp::TextEdit> bind;
    const Luau::AstLocal* alreadyBound = nullptr;

    // a pattern `qualifier` is bound with (`components.{...}`), or one destructuring it (`const .{...} = chimera`)
    // that runs at the top of the module, before anything could read through it
    for (const auto& entry : facts.patterns)
    {
        const Luau::AstDestructurePattern* pattern = entry.first;
        Luau::AstNode* holder = entry.second;
        bool owns = pattern->local == qualifier;
        bool reads = !owns && isHiddenDestructureLocal(pattern->local) && patternQualifier(*pattern, facts) == qualifier &&
                     std::find(sourceModule.root->body.begin(), sourceModule.root->body.end(), holder) != sourceModule.root->body.end() &&
                     std::none_of(facts.indexes.begin(), facts.indexes.end(),
                         [&](Luau::AstExprIndexName* index)
                         {
                             auto* object = index->expr->as<Luau::AstExprLocal>();
                             return object && object->local == qualifier && index->location.begin < holder->location.end;
                         });
        if (!owns && !reads)
            continue;

        for (const auto& field : pattern->fields)
            if (name == field.key.value)
            {
                if (field.target.location || field.asLocation || !field.target.local)
                    return;
                alreadyBound = field.target.local;
            }

        if (!alreadyBound)
        {
            auto split = splitDestructurePattern(textDocument.getText(textDocument.convertLocation(*pattern->location)));
            if (!split)
                return;
            split->second.push_back(name);
            bind = lsp::TextEdit{textDocument.convertLocation(*pattern->location),
                layOutFieldsLike(*pattern->location, split->first, split->second, textDocument)};
        }
        break;
    }

    if (!bind && !alreadyBound)
    {
        // `components` bound plainly, in a pattern or by its own `local`/`const`: it gets a pattern of its own
        auto declaration = facts.declarations.find(qualifier);
        bool plainField = std::any_of(facts.patterns.begin(), facts.patterns.end(),
            [&](const auto& entry)
            {
                for (const auto& field : entry.first->fields)
                    if (field.target.local == qualifier && !field.target.location && !field.asLocation)
                        return true;
                return false;
            });
        bool plainLocal = declaration != facts.declarations.end() && !declaration->second->isDesugaredField() && !declaration->second->destructure;
        if (qualifier->annotation || (!plainField && !plainLocal))
            return;

        Luau::Location after{qualifier->location.end, qualifier->location.end};
        bind = lsp::TextEdit{textDocument.convertLocation(after), ".{ " + name + " }"};
    }

    if (nameIsTaken(name, withType, alreadyBound, module, facts))
        return;

    auto edits = unqualifyEdits(qualifier, name, withType, facts, textDocument);
    if (!edits)
        return;
    if (bind)
        edits->push_back(*bind);
    if (edits->empty())
        return;

    addEditsAction(uri, context, "Import '" + name + "' from '" + qualifier->name.value + "'", std::move(*edits), result);
}

// The pattern under the cursor gets every field its value has that it doesn't bind yet, without going into them, and
// each `value.field` already in the file becomes `field`. Fields whose names are taken are left out.
void generateImportAllFieldsAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, const Luau::AstDestructurePattern& pattern,
    const Luau::Module& module, Luau::NotNull<Luau::BuiltinTypes> builtinTypes, const DestructureFacts& facts, const TextDocument& textDocument,
    std::vector<lsp::CodeAction>& result)
{
    const auto* table = [&]() -> const Luau::TableType*
    {
        auto ty = typeOfLocal(pattern.local, module, facts);
        return ty ? Luau::get<Luau::TableType>(Luau::follow(*ty)) : nullptr;
    }();
    auto split = splitDestructurePattern(textDocument.getText(textDocument.convertLocation(*pattern.location)));
    if (!table || !split)
        return;

    // in the order the value's fields are written, where that's known
    std::vector<std::pair<std::string, const Luau::Property*>> props;
    for (const auto& [name, prop] : table->props)
        props.emplace_back(name, &prop);
    std::stable_sort(props.begin(), props.end(),
        [](const auto& a, const auto& b)
        {
            if (!a.second->location || !b.second->location)
                return false;
            return a.second->location->begin < b.second->location->begin;
        });

    Luau::AstLocal* qualifier = patternQualifier(pattern, facts);
    std::vector<lsp::TextEdit> edits;
    size_t added = 0;

    for (const auto& entry : props)
    {
        const std::string& name = entry.first;
        const Luau::Property* prop = entry.second;
        bool bound = std::any_of(pattern.fields.begin(), pattern.fields.end(),
            [&](const auto& field)
            {
                return name == field.key.value;
            });
        bool withType = bringsAType(prop->readTy, builtinTypes);
        if (bound || nameIsTaken(name, withType, nullptr, module, facts))
            continue;

        if (qualifier)
        {
            auto uses = unqualifyEdits(qualifier, name, withType, facts, textDocument);
            if (!uses)
                continue;
            edits.insert(edits.end(), uses->begin(), uses->end());
        }

        split->second.push_back(name);
        ++added;
    }

    if (added == 0)
        return;

    edits.push_back({textDocument.convertLocation(*pattern.location), layOutFieldsLike(*pattern.location, split->first, split->second, textDocument)});
    std::string of = qualifier ? " of '" + std::string(qualifier->name.value) + "'" : "";
    addEditsAction(uri, context, "Import all fields" + of, std::move(edits), result);
}

// The reverse of importing: `Card` under the cursor in `components.{ Card }` leaves the pattern, and every use of it
// becomes `components.Card`
void generateQualifyFieldAction(const lsp::DocumentUri& uri, const lsp::CodeActionContext& context, const Luau::Position& position,
    const Luau::AstDestructurePattern& pattern, const Luau::Module& module, Luau::NotNull<Luau::BuiltinTypes> builtinTypes,
    const DestructureFacts& facts, const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    size_t fieldIndex = pattern.fields.size;
    for (size_t i = 0; i < pattern.fields.size; ++i)
    {
        const auto& field = pattern.fields.data[i];
        if (field.target.local && !field.target.location &&
            (field.keyLocation.containsClosed(position) || field.target.local->location.containsClosed(position)))
            fieldIndex = i;
    }
    Luau::AstLocal* qualifier = patternQualifier(pattern, facts);
    if (fieldIndex == pattern.fields.size || !qualifier || nameIsTaken(qualifier->name.value, false, qualifier, module, facts))
        return;

    const auto& field = pattern.fields.data[fieldIndex];
    Luau::AstLocal* local = field.target.local;
    std::string qualified = std::string(qualifier->name.value) + "." + field.key.value;
    bool withType = bringsAType(typeOfLocal(local, module, facts), builtinTypes);

    std::vector<lsp::TextEdit> edits;
    for (auto* ref : facts.localRefs)
    {
        if (ref->local != local)
            continue;
        if (facts.assigned.count(ref))
            return;
        edits.push_back({textDocument.convertLocation(ref->location), qualified});
    }
    if (withType)
        for (auto* ref : facts.typeRefs)
            if (!ref->prefix && ref->name == local->name)
                edits.push_back({textDocument.convertLocation(ref->nameLocation), qualified});

    // `components` stays bound even when `Card` was all its pattern held, since the uses now read it
    std::unordered_set<const Luau::AstLocal*> removed;
    auto removal = removeFieldsEdits({local}, facts, textDocument, &removed, {qualifier});
    if (!removed.count(local))
        return;
    edits.insert(edits.end(), removal.begin(), removal.end());

    addEditsAction(uri, context, "Qualify '" + std::string(local->name.value) + "' as '" + qualified + "'", std::move(edits), result);
}

// True when `position` sits inside the body of some function within `classStat` -- a method body, or
// a function expression used as a field's default value. Statements live there, so refactorings that
// extract statements still make sense; everywhere else in a class they don't.
struct IsInsideClassFunctionBody : Luau::AstVisitor
{
    Luau::Position targetPosition;
    bool result = false;

    explicit IsInsideClassFunctionBody(const Luau::Position& position)
        : targetPosition(position)
    {
    }

    bool visit(Luau::AstExprFunction* node) override
    {
        if (node->body && node->body->location.containsClosed(targetPosition))
            result = true;
        return true;
    }
};

bool isInsideClassFunctionBody(Luau::AstStatClass* classStat, const Luau::Position& position)
{
    IsInsideClassFunctionBody visitor(position);
    classStat->visit(&visitor);
    return visitor.result;
}

// Luwu Classes (rfcs/classes.md): access specifiers are all-or-nothing across a class. Either every
// member (and primary constructor field parameter) says `public`/`private`, or none of them do and
// the class is entirely public. This describes where a class sits between those two states, so we
// can offer to move it to whichever one it isn't in.
struct ClassAccessSpecifiers
{
    // Where a `public ` qualifier would be inserted, for each member/parameter that has none
    std::vector<Luau::Position> unqualified;
    // The span of each explicit `public ` qualifier, from the keyword up to what it qualifies
    std::vector<Luau::Location> explicitPublic;
    // Any `private` member makes both directions meaningless: the specifiers can't be dropped, and
    // the class already has to qualify everything
    bool hasPrivateMember = false;
};

// The position a member's qualifier sits in front of: the `const` modifier when there is one,
// otherwise the field name or the `function` keyword.
Luau::Position classMemberQualifierPosition(const Luau::AstClassMember& member)
{
    if (const auto* prop = member.get_if<Luau::AstClassProperty>())
        return prop->constLocation ? prop->constLocation->begin : prop->nameLocation.begin;

    // Luwu Traits (rfcs/classes/traits.md): `final` follows the access specifier, so the specifier goes in front of it
    const auto* method = member.get_if<Luau::AstClassMethod>();
    return method->finalLocation ? method->finalLocation->begin : method->keywordLocation.begin;
}

ClassAccessSpecifiers computeClassAccessSpecifiers(Luau::AstStatClass* classStat)
{
    ClassAccessSpecifiers specifiers;

    auto record = [&](const std::optional<Luau::Location>& qualifierLocation, Luau::AstClassMemberVisibility visibility, Luau::Position start)
    {
        if (!qualifierLocation)
        {
            specifiers.unqualified.push_back(start);
            return;
        }

        if (visibility == Luau::AstClassMemberVisibility::Private)
            specifiers.hasPrivateMember = true;
        else
            specifiers.explicitPublic.push_back(Luau::Location{qualifierLocation->begin, start});
    };

    std::unordered_set<std::string> membersInBody;

    for (const auto& member : classStat->members)
    {
        const Luau::Position start = classMemberQualifierPosition(member);

        if (const auto* prop = member.get_if<Luau::AstClassProperty>())
        {
            membersInBody.insert(prop->name.value);
            record(prop->qualifierLocation, prop->visibility, start);
        }
        else if (const auto* method = member.get_if<Luau::AstClassMethod>())
        {
            record(method->qualifierLocation, method->visibility, start);
        }
    }

    // A primary constructor parameter declares a field too, so it carries its own specifier -- unless
    // the class body restates that field, in which case the body member is the one that qualifies it.
    // The constructor's own qualifier (`class Account private (...)`) isn't a member specifier.
    if (const auto* primaryConstructor = classStat->primaryConstructor)
    {
        for (size_t i = 0; i < primaryConstructor->args.size; ++i)
        {
            const Luau::AstLocal* arg = primaryConstructor->args.data[i];
            if (membersInBody.find(arg->name.value) != membersInBody.end())
                continue;

            const auto& qualifiers = primaryConstructor->argsQualifiers.data[i];
            const Luau::Position start = qualifiers.constLocation ? qualifiers.constLocation->begin : arg->location.begin;
            record(qualifiers.qualifierLocation, qualifiers.visibility, start);
        }
    }

    return specifiers;
}

// "Make specifiers explicit": write `public` in front of everything that doesn't say anything yet.
std::vector<lsp::TextEdit> makeAccessSpecifiersExplicitEdits(const ClassAccessSpecifiers& specifiers, const TextDocument& textDocument)
{
    std::vector<lsp::TextEdit> edits;
    edits.reserve(specifiers.unqualified.size());

    for (const auto& position : specifiers.unqualified)
    {
        lsp::Position pos = textDocument.convertPosition(position);
        edits.push_back(lsp::TextEdit{{pos, pos}, "public "});
    }

    return edits;
}

// "Make specifiers implicit": delete every explicit `public`, leaving the terse all-public form.
std::vector<lsp::TextEdit> makeAccessSpecifiersImplicitEdits(const ClassAccessSpecifiers& specifiers, const TextDocument& textDocument)
{
    std::vector<lsp::TextEdit> edits;
    edits.reserve(specifiers.explicitPublic.size());

    for (const auto& location : specifiers.explicitPublic)
        edits.push_back(lsp::TextEdit{textDocument.convertLocation(location), ""});

    return edits;
}

void addClassAccessSpecifierAction(const lsp::DocumentUri& uri, std::string title, lsp::CodeActionKind kind, std::vector<lsp::TextEdit> edits,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    if (edits.empty())
        return;

    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = std::move(kind);
    action.isPreferred = kind == lsp::CodeActionKind::QuickFix;

    if (diagnostic)
        action.diagnostics.push_back(*diagnostic);

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));
    action.edit = workspaceEdit;

    result.push_back(action);
}

// The parser reports the all-or-nothing rule once per offending member, with a different message
// depending on how the class got there (a `private` member, a qualified constructor parameter, or
// just a mix of explicit and implicit `public`). They all share this phrasing, and they all have the
// same two possible fixes.
bool isClassAccessSpecifierSyntaxError(const std::string& message)
{
    return message.find("'public' or 'private'") != std::string::npos;
}

void generateClassAccessSpecifierFix(const lsp::DocumentUri& uri, Luau::AstStatClass* classStat, const TextDocument& textDocument,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    auto specifiers = computeClassAccessSpecifiers(classStat);

    addClassAccessSpecifierAction(uri, "Add 'public' to all implicitly public members", lsp::CodeActionKind::QuickFix,
        makeAccessSpecifiersExplicitEdits(specifiers, textDocument), diagnostic, result);

    // Dropping the specifiers only resolves the ambiguity when nothing is `private`
    if (!specifiers.hasPrivateMember)
        addClassAccessSpecifierAction(uri, "Remove 'public' from all members", lsp::CodeActionKind::QuickFix,
            makeAccessSpecifiersImplicitEdits(specifiers, textDocument), diagnostic, result);
}

// Offered anywhere inside a class, with or without a syntax error: flip the class between writing
// `public` on everything and writing it nowhere. A class with a `private` member has neither form
// available to it, so it gets nothing.
void generateClassAccessSpecifierToggle(
    const lsp::DocumentUri& uri, Luau::AstStatClass* classStat, const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    auto specifiers = computeClassAccessSpecifiers(classStat);
    if (specifiers.hasPrivateMember)
        return;

    // A class that is half-qualified is a syntax error, and the quick fixes for it already offer both
    // directions attached to the diagnostic; don't offer them a second time here.
    if (!specifiers.unqualified.empty() && !specifiers.explicitPublic.empty())
        return;

    addClassAccessSpecifierAction(uri, "Make access specifiers explicit ('public' on all members)", lsp::CodeActionKind::RefactorRewrite,
        makeAccessSpecifiersExplicitEdits(specifiers, textDocument), std::nullopt, result);

    addClassAccessSpecifierAction(uri, "Make access specifiers implicit (remove 'public' from all members)",
        lsp::CodeActionKind::RefactorRewrite, makeAccessSpecifiersImplicitEdits(specifiers, textDocument), std::nullopt, result);
}

// Find a matching diagnostic from the client-provided diagnostics by location
std::optional<lsp::Diagnostic> findMatchingDiagnostic(const std::vector<lsp::Diagnostic>& diagnostics, const lsp::Range& range)
{
    for (const auto& diag : diagnostics)
    {
        if (diag.range == range)
            return diag;
    }
    return std::nullopt;
}

void generateGlobalUsedAsLocalFix(const lsp::DocumentUri& uri, const Luau::LintWarning& lint, const TextDocument& textDocument,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    lsp::CodeAction action;
    action.title = "Add 'local' to global variable";
    action.kind = lsp::CodeActionKind::QuickFix;
    action.isPreferred = true;

    if (diagnostic)
        action.diagnostics.push_back(*diagnostic);

    lsp::Position insertPos = textDocument.convertPosition(lint.location.begin);
    lsp::TextEdit edit{{insertPos, insertPos}, "local "};

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::vector{edit});
    action.edit = workspaceEdit;

    result.push_back(action);
}

// Luwu Destructuring: the local a lint reports at `location`, when a field of a pattern binds it
const Luau::AstLocal* destructuredLocalAt(const DestructureFacts& facts, const Luau::Location& location)
{
    for (const auto& [local, _] : facts.declarations)
        if (local->location == location && isPatternField(local, facts))
            return local;
    return nullptr;
}

enum class UnusedCodeKind
{
    Variable,
    Function,
    Import
};

void generateUnusedCodeFixes(const lsp::DocumentUri& uri, const Luau::LintWarning& lint, const TextDocument& textDocument, Luau::AstStatBlock* root,
    const std::optional<lsp::Diagnostic>& diagnostic, UnusedCodeKind kind, std::vector<lsp::CodeAction>& result)
{
    lsp::Range lintRange = textDocument.convertLocation(lint.location);
    std::string name = textDocument.getText(lintRange);

    const char* kindLabel = nullptr;
    switch (kind)
    {
    case UnusedCodeKind::Variable:
        kindLabel = "variable";
        break;
    case UnusedCodeKind::Function:
        kindLabel = "function";
        break;
    case UnusedCodeKind::Import:
        kindLabel = "import";
        break;
    }

    // Fix 1: Prefix with '_' to silence the lint
    {
        lsp::CodeAction action;
        action.title = "Prefix '" + name + "' with '_' to silence";
        action.kind = lsp::CodeActionKind::QuickFix;
        action.isPreferred = false;

        if (diagnostic)
            action.diagnostics.push_back(*diagnostic);

        lsp::Position insertPos = textDocument.convertPosition(lint.location.begin);
        lsp::TextEdit edit{{insertPos, insertPos}, "_"};

        lsp::WorkspaceEdit workspaceEdit;
        workspaceEdit.changes.emplace(uri, std::vector{edit});
        action.edit = workspaceEdit;

        result.push_back(action);
    }

    // Fix 2: Delete the declaration/statement. Luwu Destructuring: a field of a pattern leaves the pattern instead.
    DestructureFacts facts;
    root->visit(&facts);
    if (const Luau::AstLocal* local = destructuredLocalAt(facts, lint.location))
    {
        auto edits = removeFieldsEdits({local}, facts, textDocument);
        if (edits.empty())
            return;

        lsp::CodeAction action;
        action.title = std::string("Remove unused ") + kindLabel + ": '" + name + "'";
        action.kind = lsp::CodeActionKind::QuickFix;
        if (diagnostic)
            action.diagnostics.push_back(*diagnostic);

        lsp::WorkspaceEdit workspaceEdit;
        workspaceEdit.changes.emplace(uri, std::move(edits));
        action.edit = workspaceEdit;
        result.push_back(action);
    }
    else if (auto statement = findStatementContainingLocal(root, lint.location);
             statement && locationOwnsItsLines(textDocument, statement->location))
    {
        lsp::CodeAction action;
        action.title = std::string("Remove unused ") + kindLabel + ": '" + name + "'";
        action.kind = lsp::CodeActionKind::QuickFix;
        action.isPreferred = false;

        if (diagnostic)
            action.diagnostics.push_back(*diagnostic);

        lsp::Range deleteRange{{statement->location.begin.line, 0}, {statement->location.end.line + 1, 0}};
        lsp::TextEdit edit{deleteRange, ""};

        lsp::WorkspaceEdit workspaceEdit;
        workspaceEdit.changes.emplace(uri, std::vector{edit});
        action.edit = workspaceEdit;

        result.push_back(action);
    }
}

void generateUnreachableCodeFix(const lsp::DocumentUri& uri, const Luau::LintWarning& lint, const TextDocument& textDocument,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    lsp::CodeAction action;
    action.title = "Remove unreachable code";
    action.kind = lsp::CodeActionKind::QuickFix;
    action.isPreferred = false;

    if (diagnostic)
        action.diagnostics.push_back(*diagnostic);

    // Delete the entire unreachable statement (the lint location is the unreachable statement)
    lsp::Range deleteRange{{lint.location.begin.line, 0}, {lint.location.end.line + 1, 0}};
    lsp::TextEdit edit{deleteRange, ""};

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::vector{edit});
    action.edit = workspaceEdit;

    result.push_back(action);
}

void generateRedundantNativeAttributeFix(const lsp::DocumentUri& uri, const Luau::LintWarning& lint, const std::optional<lsp::Diagnostic>& diagnostic,
    std::vector<lsp::CodeAction>& result)
{
    lsp::CodeAction action;
    action.title = "Remove redundant @native attribute";
    action.kind = lsp::CodeActionKind::QuickFix;
    action.isPreferred = false;

    if (diagnostic)
        action.diagnostics.push_back(*diagnostic);

    lsp::Range deleteRange{{lint.location.begin.line, 0}, {lint.location.end.line + 1, 0}};
    lsp::TextEdit edit{deleteRange, ""};

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::vector{edit});
    action.edit = workspaceEdit;

    result.push_back(action);
}

// Luwu: the `local`/`const` keyword of a binding statement, if `stat` is one
std::optional<Luau::Location> bindingKeywordLocation(Luau::AstStat* stat)
{
    if (auto local = stat->as<Luau::AstStatLocal>())
        return local->keywordLocation;
    if (auto function = stat->as<Luau::AstStatLocalFunction>())
        return function->keywordLocation;
    return std::nullopt;
}

// Luwu: swaps a binding's `local` keyword for `const` or back. Both are five letters, so the edit covers exactly the
// keyword; returns nothing if the text there isn't `from`.
std::optional<lsp::TextEdit> switchBindingKeywordEdit(
    const TextDocument& textDocument,
    Luau::AstStat* stat,
    const std::string& from,
    const std::string& to
)
{
    std::optional<Luau::Location> keyword = bindingKeywordLocation(stat);
    if (!keyword)
        return std::nullopt;

    Luau::Location range{keyword->begin, Luau::Position{keyword->begin.line, keyword->begin.column + unsigned(from.size())}};
    lsp::Range lspRange = textDocument.convertLocation(range);
    if (textDocument.getText(lspRange) != from)
        return std::nullopt;

    return lsp::TextEdit{lspRange, to};
}

void addSingleEditAction(const lsp::DocumentUri& uri, std::string title, lsp::CodeActionKind kind, bool isPreferred,
    std::vector<lsp::TextEdit> edits, const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    lsp::CodeAction action;
    action.title = std::move(title);
    action.kind = std::move(kind);
    action.isPreferred = isPreferred;

    if (diagnostic)
        action.diagnostics.push_back(*diagnostic);

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));
    action.edit = workspaceEdit;

    result.push_back(std::move(action));
}

// Luwu: ConstLocal: `local` -> `const` on the statement the warning points into
void generateConstLocalFix(const lsp::DocumentUri& uri, const Luau::LintWarning& lint, const TextDocument& textDocument, Luau::AstStatBlock* root,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    Luau::AstStat* stat = findStatementContainingLocal(root, lint.location);
    if (!stat)
        return;

    if (auto edit = switchBindingKeywordEdit(textDocument, stat, "local", "const"))
        addSingleEditAction(uri, "Change 'local' to 'const'", lsp::CodeActionKind::QuickFix, true, {*edit}, diagnostic, result);
}

// Luwu: the and-or expressions LuaAndOr reported, found by location, with what each sits in
struct FindAndOrExpressions : Luau::AstVisitor
{
    std::vector<Luau::Location> targets;
    std::vector<Luau::AstExprBinary*> found;
    // An and-or that is an operand of another expression has to keep its own parentheses once it's an if-then-else,
    // which reaches as far right as it can: `a and b or c or d` would become `if a then b else (c or d)`
    std::unordered_set<Luau::AstExprBinary*> needsParentheses;

    void noteOperand(Luau::AstExpr* operand)
    {
        if (auto binary = operand->as<Luau::AstExprBinary>())
            needsParentheses.insert(binary);
    }

    bool visit(Luau::AstExprBinary* node) override
    {
        noteOperand(node->left);
        noteOperand(node->right);

        if (std::find(targets.begin(), targets.end(), node->location) != targets.end())
            found.push_back(node);
        return true;
    }

    bool visit(Luau::AstExprUnary* node) override
    {
        noteOperand(node->expr);
        return true;
    }

    bool visit(Luau::AstExprTypeAssertion* node) override
    {
        noteOperand(node->expr);
        return true;
    }

    bool visit(Luau::AstExprIndexName* node) override
    {
        noteOperand(node->expr);
        return true;
    }

    bool visit(Luau::AstExprIndexExpr* node) override
    {
        noteOperand(node->expr);
        return true;
    }

    bool visit(Luau::AstExprCall* node) override
    {
        noteOperand(node->func);
        return true;
    }

    bool visit(Luau::AstExprIfElse* node) override
    {
        noteOperand(node->condition);
        return true;
    }
};

// Luwu: rewrites and-or expressions as if-then-else. Text is copied from the document, so an and-or nested in another
// one's operands is rewritten inside its parent's replacement, and edits never overlap.
struct AndOrRewriter
{
    const TextDocument& textDocument;
    const std::vector<Luau::AstExprBinary*>& rewrite;
    const std::unordered_set<Luau::AstExprBinary*>& needsParentheses;

    std::string text(const Luau::Position& begin, const Luau::Position& end) const
    {
        return textDocument.getText(textDocument.convertLocation(Luau::Location{begin, end}));
    }

    // `range`'s text, with every outermost and-or to rewrite inside it replaced
    std::string rewriteRange(const Luau::Location& range) const
    {
        std::string out;
        Luau::Position cursor = range.begin;

        for (Luau::AstExprBinary* node : rewrite)
        {
            bool inside = range.encloses(node->location) && !(node->location == range);
            if (!inside || node->location.begin < cursor)
                continue;

            out += text(cursor, node->location.begin);
            out += rewriteNode(node);
            cursor = node->location.end;
        }

        out += text(cursor, range.end);
        return out;
    }

    std::string rewriteNode(Luau::AstExprBinary* node) const
    {
        auto and_ = node->left->as<Luau::AstExprBinary>();
        std::string replacement = "if " + rewriteRange(and_->left->location) + " then " + rewriteRange(and_->right->location) + " else " +
                                  rewriteRange(node->right->location);

        if (needsParentheses.count(node))
            return "(" + replacement + ")";
        return replacement;
    }
};

std::vector<lsp::TextEdit> computeAndOrEdits(
    const TextDocument& textDocument,
    Luau::AstStatBlock* root,
    const std::vector<Luau::Location>& locations
)
{
    FindAndOrExpressions finder;
    finder.targets = locations;
    root->visit(&finder);

    // Outermost first, so rewriteRange meets a parent before the and-ors nested in it
    std::sort(finder.found.begin(), finder.found.end(),
        [](Luau::AstExprBinary* a, Luau::AstExprBinary* b)
        {
            if (a->location.begin != b->location.begin)
                return a->location.begin < b->location.begin;
            return b->location.end < a->location.end;
        });

    AndOrRewriter rewriter{textDocument, finder.found, finder.needsParentheses};

    std::vector<lsp::TextEdit> edits;
    Luau::Position covered{0, 0};
    bool any = false;
    for (Luau::AstExprBinary* node : finder.found)
    {
        if (!node->left->is<Luau::AstExprBinary>())
            continue;
        // nested in one already rewritten
        if (any && node->location.begin < covered)
            continue;

        edits.push_back({textDocument.convertLocation(node->location), rewriter.rewriteNode(node)});
        covered = node->location.end;
        any = true;
    }
    return edits;
}

void generateLuaAndOrFix(const lsp::DocumentUri& uri, const Luau::LintWarning& lint, const TextDocument& textDocument, Luau::AstStatBlock* root,
    const std::optional<lsp::Diagnostic>& diagnostic, std::vector<lsp::CodeAction>& result)
{
    std::vector<lsp::TextEdit> edits = computeAndOrEdits(textDocument, root, {lint.location});
    if (!edits.empty())
        addSingleEditAction(uri, "Rewrite as 'if a then b else c'", lsp::CodeActionKind::QuickFix, true, std::move(edits), diagnostic, result);
}

// Luwu: the binding statement whose keyword or names `position` is on
struct FindBindingStatementAt : Luau::AstVisitor
{
    Luau::Position position;
    Luau::AstStat* result = nullptr;

    explicit FindBindingStatementAt(const Luau::Position& position)
        : position(position)
    {
    }

    bool on(const Luau::Location& location) const
    {
        return location.begin <= position && position <= location.end;
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        bool onKeyword = node->keywordLocation && on(*node->keywordLocation);
        bool onName = std::any_of(node->vars.begin(), node->vars.end(),
            [&](Luau::AstLocal* local)
            {
                return on(local->location);
            });

        if (onKeyword || onName)
            result = node;
        return !result;
    }

    bool visit(Luau::AstStatLocalFunction* node) override
    {
        if (on(node->keywordLocation) || on(node->name->location))
        {
            result = node;
            return false;
        }
        return true;
    }
};

// Luwu: `const` <-> `local` on any binding. Switching a reassigned `local` to `const` is allowed on purpose: the errors it
// produces show what reassigns it, and switching back undoes it.
void generateBindingKeywordToggle(const lsp::DocumentUri& uri, Luau::AstStat* stat, const TextDocument& textDocument,
    std::vector<lsp::CodeAction>& result)
{
    bool isConst = false;
    if (auto local = stat->as<Luau::AstStatLocal>())
        isConst = local->isConst;
    else if (auto function = stat->as<Luau::AstStatLocalFunction>())
        isConst = function->isConst;

    const char* from = isConst ? "const" : "local";
    const char* to = isConst ? "local" : "const";
    if (auto edit = switchBindingKeywordEdit(textDocument, stat, from, to))
        addSingleEditAction(uri, std::string("Switch to '") + to + "'", lsp::CodeActionKind::RefactorRewrite, false, {*edit}, std::nullopt,
            result);
}

// Luwu Do Expressions: the keyword of a statement that leaves the block, or null
const char* leaveKeyword(Luau::AstStat* stat)
{
    if (stat->is<Luau::AstStatReturn>())
        return "return";
    if (stat->is<Luau::AstStatBreak>())
        return "break";
    if (stat->is<Luau::AstStatContinue>())
        return "continue";
    return nullptr;
}

// The innermost guard `if <condition> then return/break/continue end` around `position`
struct FindGuardAt : Luau::AstVisitor
{
    Luau::Position position;
    Luau::AstStatIf* result = nullptr;

    explicit FindGuardAt(const Luau::Position& position)
        : position(position)
    {
    }

    bool visit(Luau::AstStatIf* node) override
    {
        if (!node->location.containsClosed(position))
            return false;

        bool guardShape = !node->elsebody && node->clauses.size == 0 && !node->isWhenGuard && node->thenbody->body.size == 1 &&
                          leaveKeyword(node->thenbody->body.data[0]);
        if (guardShape)
            result = node;
        return true;
    }
};

Luau::AstExpr* stripGroups(Luau::AstExpr* expr)
{
    while (auto group = expr->as<Luau::AstExprGroup>())
        expr = group->expr;
    return expr;
}

// A statement has to start with a name, or a call or index on one, for `x or return` to parse as one: `not` can't start
// a statement, and a `(` would continue the line before it as a call
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
        else if (auto instantiate = expr->as<Luau::AstExprInstantiate>())
            expr = instantiate->expr;
        else
            return expr->is<Luau::AstExprLocal>() || expr->is<Luau::AstExprGlobal>();
    }
}

// An if-then-else expression at the end would take the `or return` into its `else` branch
bool endsWithIfElse(Luau::AstExpr* expr)
{
    while (true)
    {
        if (auto binary = expr->as<Luau::AstExprBinary>())
            expr = binary->right;
        else if (auto unary = expr->as<Luau::AstExprUnary>())
            expr = unary->expr;
        else
            return expr->is<Luau::AstExprIfElse>();
    }
}

// The condition `x or return` needs in place of the guard's: `not x` gives `x`, and `==` and `~=` swap. Anything else
// would need a `not` in front, which can't start a statement.
std::optional<std::string> guardCondition(const TextDocument& textDocument, Luau::AstExpr* condition)
{
    auto text = [&](Luau::AstExpr* expr)
    {
        return textDocument.getText(textDocument.convertLocation(expr->location));
    };

    condition = stripGroups(condition);

    Luau::AstExpr* guard = nullptr;
    std::string guardText;
    if (auto unary = condition->as<Luau::AstExprUnary>(); unary && unary->op == Luau::AstExprUnary::Op::Not)
    {
        // `or` binds loosest, so whatever was in the parentheses needs none now
        guard = stripGroups(unary->expr);
        guardText = text(guard);
    }
    else if (auto binary = condition->as<Luau::AstExprBinary>();
             binary && (binary->op == Luau::AstExprBinary::CompareEq || binary->op == Luau::AstExprBinary::CompareNe))
    {
        guard = binary;
        guardText = text(binary->left) + (binary->op == Luau::AstExprBinary::CompareEq ? " ~= " : " == ") + text(binary->right);
    }

    if (!guard || !startsWithName(guard) || endsWithIfElse(guard))
        return std::nullopt;
    return guardText;
}

// Luwu Do Expressions: `if not x then return y end` -> `x` with `or return y` on the next line, one level in
void generateOrLeaveGuardRewrite(const lsp::DocumentUri& uri, Luau::AstStatIf* node, const Luau::SourceModule& sourceModule,
    const TextDocument& textDocument, std::vector<lsp::CodeAction>& result)
{
    // an `elseif` is part of the `if` before it
    if (textDocument.getText(textDocument.convertLocation(node->ifLocation)) != "if")
        return;
    if (!locationOwnsItsLines(textDocument, node->location))
        return;

    // the rewrite is rebuilt from the condition and the leave, so a comment anywhere else in it would be lost
    for (const auto& comment : sourceModule.commentLocations)
    {
        if (node->location.encloses(comment.location))
            return;
    }

    std::optional<std::string> condition = guardCondition(textDocument, node->condition);
    if (!condition)
        return;

    Luau::AstStat* leave = node->thenbody->body.data[0];
    std::string leaveText = textDocument.getText(textDocument.convertLocation(leave->location));
    // a bare `return` needs a `;` and several values need parentheses, or the parser would read on past them
    if (auto ret = leave->as<Luau::AstStatReturn>())
    {
        if (ret->list.size == 0)
            leaveText = "return;";
        else if (ret->list.size > 1)
            leaveText = "(" + leaveText + ")";
    }

    std::string indent = getLineIndentation(textDocument, node->location.begin.line);
    std::string newText = *condition + "\n" + indent + detectIndentUnit(textDocument) + "or " + leaveText;

    addSingleEditAction(uri, std::string("Rewrite as 'or ") + leaveKeyword(leave) + "' guard", lsp::CodeActionKind::RefactorRewrite, false,
        {{textDocument.convertLocation(node->location), newText}}, std::nullopt, result);
}
} // namespace

lsp::WorkspaceEdit WorkspaceFolder::computeOrganiseRequiresEdit(const lsp::DocumentUri& uri)
{
    auto moduleName = fileResolver.getModuleName(uri);
    auto textDocument = fileResolver.getTextDocument(uri);

    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + uri.toString());

    frontend.parse(moduleName);

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return {};

    // Find all the `local X = require(...)` calls
    Luau::LanguageServer::AutoImports::FindImportsVisitor visitor;
    visitor.visit(sourceModule->root);

    // Check if there are any requires
    if (visitor.requiresMap.size() == 1 && visitor.requiresMap.back().empty())
        return {};

    // Treat each require group individually
    std::vector<lsp::TextEdit> edits;
    for (const auto& requireGroup : visitor.requiresMap)
    {
        if (requireGroup.empty())
            continue;

        // Find the first line of the group
        size_t firstRequireLine = requireGroup.begin()->second->location.begin.line;
        Luau::Location previousRequireLocation{{0, 0}, {0, 0}};
        bool isSorted = true;
        for (const auto& [_, stat] : requireGroup)
        {
            if (stat->location.begin < previousRequireLocation.begin)
                isSorted = false;
            previousRequireLocation = stat->location;
            firstRequireLine = stat->location.begin.line < firstRequireLine ? stat->location.begin.line : firstRequireLine;
        }

        // Test to see that if all the requires are already sorted -> if they are, then just leave alone
        // to prevent clogging the undo history stack
        if (isSorted)
            continue;

        // We firstly delete all the previous requires, as they will be added later
        for (const auto& [_, stat] : requireGroup)
            edits.emplace_back(lsp::TextEdit{{{stat->location.begin.line, 0}, {stat->location.begin.line + 1, 0}}, ""});

        // We find the first line to add these services to, and then add them in sorted order
        lsp::Range insertLocation{{firstRequireLine, 0}, {firstRequireLine, 0}};
        for (const auto& [serviceName, stat] : requireGroup)
        {
            // We need to rewrite the statement as we expected it
            auto importText = Luau::toString(stat) + "\n";
            edits.emplace_back(lsp::TextEdit{insertLocation, importText});
        }
    }

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, edits);
    return workspaceEdit;
}

lsp::CodeActionResult WorkspaceFolder::codeAction(const lsp::CodeActionParams& params, const LSPCancellationToken& cancellationToken)
{
    std::vector<lsp::CodeAction> result;

    if (!params.context.wants(lsp::CodeActionKind::QuickFix) && !params.context.wants(lsp::CodeActionKind::Source) &&
        !params.context.wants(lsp::CodeActionKind::SourceOrganizeImports) && !params.context.wants(lsp::CodeActionKind::RefactorExtract) &&
        !params.context.wants(lsp::CodeActionKind::RefactorInline) && !params.context.wants(lsp::CodeActionKind::RefactorRewrite))
        return result;

    auto config = client->getConfiguration(rootUri);
    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);

    if (!textDocument)
        return result;

    Luau::CheckResult cr =
        FFlag::LuauSolverV2 ? checkStrict(moduleName, cancellationToken, /* forAutocomplete= */ false) : checkSimple(moduleName, cancellationToken);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return result;

    auto requestRange = textDocument->convertRange(params.range);

    // Laying out the parameter or trait list under the cursor goes first, so it heads the menu
    if (params.context.wants(lsp::CodeActionKind::QuickFix) || params.context.wants(lsp::CodeActionKind::RefactorRewrite))
    {
        FindParameterListAtPosition parameterListFinder(requestRange.begin);
        sourceModule->root->visit(&parameterListFinder);
        if (parameterListFinder.result)
            generateParameterListWrapAction(params.textDocument.uri, params.context, *parameterListFinder.result, *textDocument, result);

        FindTraitListAtPosition traitListFinder(requestRange.begin, *textDocument);
        sourceModule->root->visit(&traitListFinder);
        if (traitListFinder.result)
            generateTraitListWrapAction(params.textDocument.uri, params.context, *traitListFinder.result, *textDocument, result);

        FindDestructurePatternAtPosition destructureFinder(requestRange.begin);
        sourceModule->root->visit(&destructureFinder);
        if (destructureFinder.result)
            generateDestructureLayoutAction(params.textDocument.uri, params.context, *destructureFinder.result, *textDocument, result);

        // Luwu Destructuring: importing fields into a pattern and qualifying them back
        if (auto module = getModule(moduleName))
        {
            DestructureFacts facts;
            sourceModule->root->visit(&facts);
            auto builtinTypes = frontend.builtinTypes;

            generateImportFieldAction(params.textDocument.uri, params.context, requestRange.begin, *sourceModule, *module, builtinTypes, facts,
                *textDocument, result);

            if (destructureFinder.result && destructureFinder.result->pattern)
            {
                const Luau::AstDestructurePattern& pattern = *destructureFinder.result->pattern;
                generateQualifyFieldAction(
                    params.textDocument.uri, params.context, requestRange.begin, pattern, *module, builtinTypes, facts, *textDocument, result);
                generateImportAllFieldsAction(params.textDocument.uri, params.context, pattern, *module, builtinTypes, facts, *textDocument, result);
            }
        }
    }

    // Quick fixes from lint warnings
    if (params.context.wants(lsp::CodeActionKind::QuickFix))
    {
        for (const auto& lint : cr.lintResult.warnings)
        {
            if (!requestRange.overlaps(lint.location))
                continue;

            auto lintRange = textDocument->convertLocation(lint.location);
            auto diagnostic = findMatchingDiagnostic(params.context.diagnostics, lintRange);

            switch (lint.code)
            {
            case Luau::LintWarning::Code_GlobalUsedAsLocal:
                generateGlobalUsedAsLocalFix(params.textDocument.uri, lint, *textDocument, diagnostic, result);
                break;
            case Luau::LintWarning::Code_LocalUnused:
                generateUnusedCodeFixes(
                    params.textDocument.uri, lint, *textDocument, sourceModule->root, diagnostic, UnusedCodeKind::Variable, result);
                break;
            case Luau::LintWarning::Code_FunctionUnused:
                generateUnusedCodeFixes(
                    params.textDocument.uri, lint, *textDocument, sourceModule->root, diagnostic, UnusedCodeKind::Function, result);
                break;
            case Luau::LintWarning::Code_ImportUnused:
                generateUnusedCodeFixes(params.textDocument.uri, lint, *textDocument, sourceModule->root, diagnostic, UnusedCodeKind::Import, result);
                break;
            case Luau::LintWarning::Code_UnreachableCode:
                generateUnreachableCodeFix(params.textDocument.uri, lint, *textDocument, diagnostic, result);
                break;
            case Luau::LintWarning::Code_RedundantNativeAttribute:
                generateRedundantNativeAttributeFix(params.textDocument.uri, lint, diagnostic, result);
                break;
            case Luau::LintWarning::Code_ConstLocal:
                generateConstLocalFix(params.textDocument.uri, lint, *textDocument, sourceModule->root, diagnostic, result);
                break;
            case Luau::LintWarning::Code_LuaAndOr:
                generateLuaAndOrFix(params.textDocument.uri, lint, *textDocument, sourceModule->root, diagnostic, result);
                break;
            default:
                break;
            }
        }

        UnknownSymbolFixContext unknownSymbolCtx{
            params.textDocument.uri,
            Luau::NotNull(textDocument),
            Luau::NotNull(sourceModule),
            Luau::NotNull(this),
        };

        std::unordered_set<Luau::AstStatClass*> fixedAccessSpecifiersForClasses;

        for (const auto& error : cr.errors)
        {
            if (!requestRange.overlaps(error.location))
                continue;

            lsp::Range errorRange = textDocument->convertLocation(error.location);
            auto diagnostic = findMatchingDiagnostic(params.context.diagnostics, errorRange);

            if (const auto* unknownSymbol = Luau::get_if<Luau::UnknownSymbol>(&error.data))
            {
                // Luwu: a misspelled variable is replaced by the name it was probably meant to be
                if (unknownSymbol->unbound == Luau::UnknownSymbol::Unbound::Typo)
                {
                    lsp::CodeAction action;
                    action.title = "Change '" + unknownSymbol->name + "' to '" + unknownSymbol->meant + "'";
                    action.kind = lsp::CodeActionKind::QuickFix;
                    action.isPreferred = true;
                    if (diagnostic)
                        action.diagnostics.push_back(*diagnostic);

                    lsp::WorkspaceEdit workspaceEdit;
                    workspaceEdit.changes.emplace(params.textDocument.uri, std::vector{lsp::TextEdit{errorRange, unknownSymbol->meant}});
                    action.edit = workspaceEdit;
                    result.push_back(action);
                }

                // Luwu Destructuring: a name a destructuring could bring in, by adding it to one
                if (auto module = getModule(moduleName))
                {
                    bool typesOnly = unknownSymbol->context == Luau::UnknownSymbol::Type;
                    for (auto& candidate : destructureImportCandidates(
                             *sourceModule, *module, error.location.begin, unknownSymbol->name, typesOnly, config, *textDocument))
                    {
                        if (candidate.name != unknownSymbol->name)
                            continue;

                        lsp::CodeAction action;
                        action.title = "Import '" + candidate.name + "' from '" + candidate.from + "'";
                        action.kind = lsp::CodeActionKind::QuickFix;
                        if (diagnostic)
                            action.diagnostics.push_back(*diagnostic);

                        lsp::WorkspaceEdit workspaceEdit;
                        workspaceEdit.changes.emplace(params.textDocument.uri, std::vector{std::move(candidate.edit)});
                        action.edit = workspaceEdit;
                        result.push_back(action);
                    }
                }

                platform->handleUnknownSymbolFix(unknownSymbolCtx, *unknownSymbol, diagnostic, result);
            }
            else if (const auto* misspelledProp = Luau::get_if<Luau::UnknownPropButFoundLikeProp>(&error.data))
            {
                bool singleCandidate = misspelledProp->candidates.size() == 1;

                for (const auto& candidate : misspelledProp->candidates)
                {
                    lsp::CodeAction action;
                    action.title = "Change '" + misspelledProp->key + "' to '" + candidate + "'";
                    action.kind = lsp::CodeActionKind::QuickFix;
                    action.isPreferred = singleCandidate;

                    if (diagnostic)
                        action.diagnostics.push_back(*diagnostic);

                    lsp::TextEdit edit{errorRange, candidate};

                    lsp::WorkspaceEdit workspaceEdit;
                    workspaceEdit.changes.emplace(params.textDocument.uri, std::vector{edit});
                    action.edit = workspaceEdit;

                    result.push_back(action);
                }
            }
            else if (const auto* syntaxError = Luau::get_if<Luau::SyntaxError>(&error.data))
            {
                if (isClassAccessSpecifierSyntaxError(syntaxError->message))
                {
                    // The same class is reported once per offending member; only fix it once
                    if (auto* classStat = types::findEnclosingClassStat(sourceModule->root, error.location.begin);
                        classStat && fixedAccessSpecifiersForClasses.insert(classStat).second)
                        generateClassAccessSpecifierFix(params.textDocument.uri, classStat, *textDocument, diagnostic, result);
                }
            }
        }
    }

    // Source actions
    if (params.context.wants(lsp::CodeActionKind::Source) || params.context.wants(lsp::CodeActionKind::SourceOrganizeImports))
    {
        // Add sort requires code action
        lsp::CodeAction organiseImportsAction;
        organiseImportsAction.title = "Sort requires";
        organiseImportsAction.kind = lsp::CodeActionKind::SourceOrganizeImports;

        // TODO: support resolving and defer computation till later
        // if (client->capabilities.textDocument && client->capabilities.textDocument->codeAction &&
        //     client->capabilities.textDocument->codeAction->resolveSupport &&
        //     contains(client->capabilities.textDocument->codeAction->resolveSupport->properties, "edit"))
        organiseImportsAction.edit = computeOrganiseRequiresEdit(params.textDocument.uri);
        result.emplace_back(organiseImportsAction);

        // Add "Remove all unused code" source action
        std::vector<lsp::TextEdit> edits;
        std::unordered_set<size_t> deletedLines;

        // Luwu Destructuring: unused fields leave their patterns, a pattern's together in one edit
        DestructureFacts facts;
        sourceModule->root->visit(&facts);
        std::unordered_set<const Luau::AstLocal*> unusedFields;

        for (const auto& lint : cr.lintResult.warnings)
        {
            if (lint.code == Luau::LintWarning::Code_LocalUnused || lint.code == Luau::LintWarning::Code_FunctionUnused ||
                lint.code == Luau::LintWarning::Code_ImportUnused)
            {
                if (const Luau::AstLocal* field = destructuredLocalAt(facts, lint.location))
                {
                    unusedFields.insert(field);
                    continue;
                }

                auto statement = findStatementContainingLocal(sourceModule->root, lint.location);
                if (statement && locationOwnsItsLines(*textDocument, statement->location))
                {
                    size_t startLine = statement->location.begin.line;
                    if (deletedLines.find(startLine) == deletedLines.end())
                    {
                        lsp::Range deleteRange{{statement->location.begin.line, 0}, {statement->location.end.line + 1, 0}};
                        edits.push_back({deleteRange, ""});

                        for (size_t line = statement->location.begin.line; line <= statement->location.end.line; ++line)
                            deletedLines.insert(line);
                    }
                }
            }
            else if (lint.code == Luau::LintWarning::Code_UnreachableCode && locationOwnsItsLines(*textDocument, lint.location))
            {
                size_t startLine = lint.location.begin.line;
                if (deletedLines.find(startLine) == deletedLines.end())
                {
                    lsp::Range deleteRange{{lint.location.begin.line, 0}, {lint.location.end.line + 1, 0}};
                    edits.push_back({deleteRange, ""});

                    for (size_t line = lint.location.begin.line; line <= lint.location.end.line; ++line)
                        deletedLines.insert(line);
                }
            }
        }

        auto fieldEdits = removeFieldsEdits(unusedFields, facts, *textDocument);
        edits.insert(edits.end(), fieldEdits.begin(), fieldEdits.end());

        if (!edits.empty())
        {
            lsp::CodeAction removeUnusedAction;
            removeUnusedAction.title = "Remove all unused code";
            removeUnusedAction.kind = lsp::CodeActionKind::Source;

            lsp::WorkspaceEdit workspaceEdit;
            workspaceEdit.changes.emplace(params.textDocument.uri, edits);
            removeUnusedAction.edit = workspaceEdit;

            result.emplace_back(removeUnusedAction);
        }

        // Luwu: fix every ConstLocal and every LuaAndOr in the module
        {
            std::vector<lsp::TextEdit> constEdits;
            std::vector<Luau::Location> andOrLocations;
            for (const auto& lint : cr.lintResult.warnings)
            {
                if (lint.code == Luau::LintWarning::Code_ConstLocal)
                {
                    if (auto stat = findStatementContainingLocal(sourceModule->root, lint.location))
                        if (auto edit = switchBindingKeywordEdit(*textDocument, stat, "local", "const"))
                            constEdits.push_back(*edit);
                }
                else if (lint.code == Luau::LintWarning::Code_LuaAndOr)
                    andOrLocations.push_back(lint.location);
            }

            if (!constEdits.empty())
                addSingleEditAction(params.textDocument.uri, "Make all never-reassigned locals 'const'", lsp::CodeActionKind::Source, false,
                    std::move(constEdits), std::nullopt, result);

            if (!andOrLocations.empty())
            {
                std::vector<lsp::TextEdit> andOrEdits = computeAndOrEdits(*textDocument, sourceModule->root, andOrLocations);
                if (!andOrEdits.empty())
                    addSingleEditAction(params.textDocument.uri, "Rewrite all 'a and b or c' as 'if a then b else c'",
                        lsp::CodeActionKind::Source, false, std::move(andOrEdits), std::nullopt, result);
            }
        }

        // Add "Add all missing requires" source action
        if (!cr.errors.empty())
        {
            UnknownSymbolFixContext ctx{
                params.textDocument.uri,
                Luau::NotNull(textDocument),
                Luau::NotNull(sourceModule),
                Luau::NotNull(this),
            };

            auto importEdits = platform->computeAddAllMissingImportsEdits(ctx, cr.errors);
            if (!importEdits.empty())
            {
                lsp::CodeAction addMissingRequiresAction;
                addMissingRequiresAction.title = "Add all missing requires";
                addMissingRequiresAction.kind = lsp::CodeActionKind::Source;

                lsp::WorkspaceEdit workspaceEdit;
                workspaceEdit.changes.emplace(params.textDocument.uri, importEdits);
                addMissingRequiresAction.edit = workspaceEdit;

                result.emplace_back(addMissingRequiresAction);
            }
        }
    }

    // Refactoring actions. A class body holds member declarations rather than statements, so
    // extracting there produces nothing useful and hoists code out of the class; suppress it
    // everywhere in a class except inside a function body, where statements do live.
    if (params.context.wants(lsp::CodeActionKind::RefactorExtract) || params.context.wants(lsp::CodeActionKind::RefactorInline))
    {
        auto* enclosingClass = types::findEnclosingClassStat(sourceModule->root, requestRange.begin);
        if (!enclosingClass || isInsideClassFunctionBody(enclosingClass, requestRange.begin))
            computeRefactorings(params, *sourceModule, *textDocument, requestRange, result);
    }

    if (params.context.wants(lsp::CodeActionKind::RefactorExtract))
        computeClassRefactorings(params, *sourceModule, *textDocument, requestRange, result);

    if (params.context.wants(lsp::CodeActionKind::RefactorRewrite))
    {
        if (auto* classStat = types::findEnclosingClassStat(sourceModule->root, requestRange.begin))
            generateClassAccessSpecifierToggle(params.textDocument.uri, classStat, *textDocument, result);

        // Luwu: `local` <-> `const` on the binding under the cursor
        FindBindingStatementAt bindingFinder(requestRange.begin);
        sourceModule->root->visit(&bindingFinder);
        if (bindingFinder.result)
            generateBindingKeywordToggle(params.textDocument.uri, bindingFinder.result, *textDocument, result);

        // Luwu: `if not x then return end` -> `x or return`
        if (FFlag::LuwuDoExpr)
        {
            FindGuardAt guardFinder(requestRange.begin);
            sourceModule->root->visit(&guardFinder);
            if (guardFinder.result)
                generateOrLeaveGuardRewrite(params.textDocument.uri, guardFinder.result, *sourceModule, *textDocument, result);
        }
    }

    platform->handleCodeAction(params, result);

    return result;
}

lsp::CodeAction WorkspaceFolder::codeActionResolve(const lsp::CodeAction& action, const LSPCancellationToken& cancellationToken)
{
    return resolveRefactoring(action, *this, cancellationToken);
}
