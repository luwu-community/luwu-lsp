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

#include <unordered_set>

LUAU_FASTFLAG(LuauSolverV2)

namespace
{
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

// The leading whitespace of a source line, used to match the document's existing indentation
std::string getLineIndentation(const TextDocument& textDocument, size_t line)
{
    std::string lineText = textDocument.getLine(line);
    size_t indent = 0;
    while (indent < lineText.size() && (lineText[indent] == ' ' || lineText[indent] == '\t'))
        ++indent;
    return lineText.substr(0, indent);
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

// Splits a parameter list's contents on top-level commas, so commas inside a parameter's own type
// (a table type, a generic argument list, a function type) don't split it.
std::vector<std::string> splitParameters(const std::string& parameters)
{
    std::vector<std::string> result;
    int depth = 0;
    char stringDelimiter = '\0';
    size_t start = 0;

    for (size_t i = 0; i < parameters.size(); ++i)
    {
        char c = parameters[i];

        if (stringDelimiter != '\0')
        {
            if (c == '\\')
                ++i;
            else if (c == stringDelimiter)
                stringDelimiter = '\0';
            continue;
        }

        if (c == '"' || c == '\'')
            stringDelimiter = c;
        else if (c == '(' || c == '{' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == '}' || c == ']' || c == '>')
            --depth;
        else if (c == ',' && depth == 0)
        {
            result.push_back(parameters.substr(start, i - start));
            start = i + 1;
        }
    }

    result.push_back(parameters.substr(start));

    for (auto& parameter : result)
        trim(parameter);

    return result;
}

// One level of indentation, as the document already uses it: the leading whitespace of the first
// indented line, falling back to four spaces.
std::string detectIndentUnit(const TextDocument& textDocument)
{
    for (size_t line = 0; line < textDocument.lineCount(); ++line)
    {
        std::string lineText = textDocument.getLine(line);
        size_t indent = 0;
        while (indent < lineText.size() && (lineText[indent] == ' ' || lineText[indent] == '\t'))
            ++indent;

        // Skip blank (or whitespace-only) lines, and continuation lines of an already-wrapped construct
        if (indent == 0 || indent == lineText.size())
            continue;

        if (lineText[0] == '\t')
            return "\t";

        return std::string(indent, ' ');
    }

    return "    ";
}

// Rewrites a parameter list between one-per-line (Rust style, closing paren back at the function's
// own indentation) and all-on-one-line. Offered on any function, method or primary constructor.
void generateParameterListWrapAction(const lsp::DocumentUri& uri, const Luau::Location& argLocation, const TextDocument& textDocument,
    std::vector<lsp::CodeAction>& result)
{
    lsp::Range range = textDocument.convertLocation(argLocation);
    std::string text = textDocument.getText(range);
    if (text.size() < 2 || text.front() != '(' || text.back() != ')')
        return;

    auto parameters = splitParameters(text.substr(1, text.size() - 2));
    if (parameters.empty() || (parameters.size() == 1 && parameters[0].empty()))
        return;

    const bool isMultiline = text.find('\n') != std::string::npos;

    std::string newText;
    std::string title;

    if (isMultiline)
    {
        title = "Put parameters on one line";
        newText = "(";
        for (size_t i = 0; i < parameters.size(); ++i)
            newText += (i == 0 ? "" : ", ") + parameters[i];
        newText += ")";
    }
    else
    {
        title = "Put each parameter on its own line";
        std::string baseIndent = getLineIndentation(textDocument, argLocation.begin.line);
        std::string parameterIndent = baseIndent + detectIndentUnit(textDocument);

        newText = "(\n";
        for (size_t i = 0; i < parameters.size(); ++i)
            newText += parameterIndent + parameters[i] + (i + 1 == parameters.size() ? "\n" : ",\n");
        newText += baseIndent + ")";
    }

    lsp::CodeAction action;
    action.title = title;
    action.kind = lsp::CodeActionKind::RefactorRewrite;

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::vector{lsp::TextEdit{range, newText}});
    action.edit = workspaceEdit;

    result.push_back(action);
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

    return member.get_if<Luau::AstClassMethod>()->keywordLocation.begin;
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

    // Fix 2: Delete the declaration/statement
    if (auto statement = findStatementContainingLocal(root, lint.location))
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

        for (const auto& lint : cr.lintResult.warnings)
        {
            if (lint.code == Luau::LintWarning::Code_LocalUnused || lint.code == Luau::LintWarning::Code_FunctionUnused ||
                lint.code == Luau::LintWarning::Code_ImportUnused)
            {
                if (auto statement = findStatementContainingLocal(sourceModule->root, lint.location))
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
            else if (lint.code == Luau::LintWarning::Code_UnreachableCode)
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

    if (params.context.wants(lsp::CodeActionKind::RefactorRewrite))
    {
        FindParameterListAtPosition parameterListFinder(requestRange.begin);
        sourceModule->root->visit(&parameterListFinder);
        if (parameterListFinder.result)
            generateParameterListWrapAction(params.textDocument.uri, *parameterListFinder.result, *textDocument, result);

        if (auto* classStat = types::findEnclosingClassStat(sourceModule->root, requestRange.begin))
            generateClassAccessSpecifierToggle(params.textDocument.uri, classStat, *textDocument, result);
    }

    platform->handleCodeAction(params, result);

    return result;
}

lsp::CodeAction WorkspaceFolder::codeActionResolve(const lsp::CodeAction& action, const LSPCancellationToken& cancellationToken)
{
    return resolveRefactoring(action, *this, cancellationToken);
}
