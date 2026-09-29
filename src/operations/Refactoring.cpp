#include "LSP/Refactoring.hpp"
#include "LSP/Workspace.hpp"
#include "LSP/LuauExt.hpp"
#include "Luau/AstQuery.h"

#include <algorithm>
#include <unordered_set>

LUAU_FASTFLAG(LuauSolverV2)

namespace
{

// Returns nullptr if the selection doesn't align with an expression boundary.
Luau::AstExpr* findExprCoveringRange(const Luau::SourceModule& sourceModule, const Luau::Location& range)
{
    auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, range.begin);
    if (ancestry.empty())
        return nullptr;

    Luau::AstExpr* best = nullptr;
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
    {
        if (auto* expr = (*it)->asExpr())
        {
            // The expression must fully contain the selection
            if (expr->location.encloses(range))
            {
                // Prefer the tightest fit
                if (!best || best->location.encloses(expr->location))
                    best = expr;
            }
        }
    }

    return best;
}

Luau::AstStatBlock* findEnclosingBlock(const Luau::SourceModule& sourceModule, const Luau::Position& pos)
{
    auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, pos);
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
    {
        if (auto* block = (*it)->as<Luau::AstStatBlock>())
            return block;
    }
    return nullptr;
}

Luau::AstStat* findEnclosingStatement(const Luau::SourceModule& sourceModule, Luau::AstExpr* expr)
{
    auto ancestry = Luau::findAstAncestryOfPosition(sourceModule, expr->location.begin);
    for (auto it = ancestry.rbegin(); it != ancestry.rend(); ++it)
    {
        if (auto* stat = (*it)->asStat(); stat && !stat->as<Luau::AstStatBlock>())
            return stat;
    }
    return nullptr;
}

struct StatementRange
{
    size_t start = 0;
    size_t count = 0;
};

StatementRange findStatementsInRange(Luau::AstStatBlock* block, const Luau::Location& range)
{
    StatementRange result;
    bool started = false;

    for (size_t i = 0; i < block->body.size; ++i)
    {
        Luau::AstStat* stat = block->body.data[i];
        bool overlaps = range.overlaps(stat->location);

        if (!started && overlaps)
        {
            // For the first and last statements, allow partial overlap
            // but the statement should be mostly within the selection
            result.start = i;
            result.count = 1;
            started = true;
        }
        else if (started && overlaps)
        {
            result.count = i - result.start + 1;
        }
        else if (started && !overlaps)
        {
            break;
        }
    }

    return result;
}

struct FreeVariableVisitor : Luau::AstVisitor
{
    std::unordered_set<Luau::AstLocal*> defined;
    std::vector<Luau::AstLocal*> referenced; // ordered by first occurrence
    std::unordered_set<Luau::AstLocal*> referencedSet;
    bool hasControlFlowEscape = false;
    int loopDepth = 0;

    // Don't descend into nested function bodies — their locals, references,
    // and control flow are scoped to the inner function, not to our selection.
    bool visit(Luau::AstExprFunction*) override
    {
        return false;
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
            defined.insert(node->vars.data[i]);
        return true;
    }

    bool visit(Luau::AstStatLocalFunction* node) override
    {
        defined.insert(node->name);
        // Don't descend — the function body is a nested scope
        return false;
    }

    bool visit(Luau::AstStatFor* node) override
    {
        defined.insert(node->var);
        loopDepth++;
        node->from->visit(this);
        node->to->visit(this);
        if (node->step)
            node->step->visit(this);
        node->body->visit(this);
        loopDepth--;
        return false;
    }

    bool visit(Luau::AstStatForIn* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
            defined.insert(node->vars.data[i]);
        loopDepth++;
        for (size_t i = 0; i < node->values.size; ++i)
            node->values.data[i]->visit(this);
        node->body->visit(this);
        loopDepth--;
        return false;
    }

    bool visit(Luau::AstStatWhile* node) override
    {
        loopDepth++;
        node->condition->visit(this);
        node->body->visit(this);
        loopDepth--;
        return false;
    }

    bool visit(Luau::AstStatRepeat* node) override
    {
        loopDepth++;
        node->body->visit(this);
        node->condition->visit(this);
        loopDepth--;
        return false;
    }

    bool visit(Luau::AstExprLocal* node) override
    {
        if (referencedSet.insert(node->local).second)
            referenced.push_back(node->local);
        return true;
    }

    bool visit(Luau::AstStatReturn*) override
    {
        hasControlFlowEscape = true;
        return true;
    }

    bool visit(Luau::AstStatBreak*) override
    {
        // break inside a loop within the selection is fine
        if (loopDepth == 0)
            hasControlFlowEscape = true;
        return true;
    }

    bool visit(Luau::AstStatContinue*) override
    {
        // continue inside a loop within the selection is fine
        if (loopDepth == 0)
            hasControlFlowEscape = true;
        return true;
    }

    // Free variables = referenced but not defined in the selection
    std::vector<Luau::AstLocal*> getFreeVariables() const
    {
        std::vector<Luau::AstLocal*> result;
        for (auto* local : referenced)
        {
            if (defined.find(local) == defined.end())
                result.push_back(local);
        }
        return result;
    }
};

struct IsReassignedVisitor : Luau::AstVisitor
{
    Luau::AstLocal* target;
    bool reassigned = false;

    explicit IsReassignedVisitor(Luau::AstLocal* target)
        : target(target)
    {
    }

    bool visit(Luau::AstStatAssign* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
        {
            if (auto* local = node->vars.data[i]->as<Luau::AstExprLocal>())
            {
                if (local->local == target)
                {
                    reassigned = true;
                    return false;
                }
            }
        }
        return true;
    }

    bool visit(Luau::AstStatCompoundAssign* node) override
    {
        if (auto* local = node->var->as<Luau::AstExprLocal>())
        {
            if (local->local == target)
            {
                reassigned = true;
                return false;
            }
        }
        return true;
    }
};

struct FindDeclaration : Luau::AstVisitor
{
    Luau::AstLocal* target;
    Luau::AstStatLocal* result = nullptr;

    explicit FindDeclaration(Luau::AstLocal* target)
        : target(target)
    {
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        for (size_t i = 0; i < node->vars.size; ++i)
        {
            if (node->vars.data[i] == target)
            {
                result = node;
                return false;
            }
        }
        return true;
    }
};

bool needsParentheses(Luau::AstExpr* expr)
{
    return expr->is<Luau::AstExprBinary>() || expr->is<Luau::AstExprUnary>() || expr->is<Luau::AstExprIfElse>() ||
           expr->is<Luau::AstExprTypeAssertion>();
}

std::string getLineIndentation(const TextDocument& textDocument, size_t line)
{
    std::string lineText = textDocument.getLine(line);
    size_t indent = 0;
    while (indent < lineText.size() && (lineText[indent] == ' ' || lineText[indent] == '\t'))
        ++indent;
    return lineText.substr(0, indent);
}

struct RefactoringResult
{
    lsp::WorkspaceEdit edit;
    std::optional<lsp::Position> renamePosition;
};

std::optional<RefactoringResult> computeExtractVariableEdit(
    const lsp::DocumentUri& uri,
    const Luau::SourceModule& sourceModule,
    const TextDocument& textDocument,
    const lsp::Range& range)
{
    auto luauRange = textDocument.convertRange(range);
    auto* expr = findExprCoveringRange(sourceModule, luauRange);
    if (!expr)
        return std::nullopt;

    // Don't extract simple local/global references
    if (expr->is<Luau::AstExprLocal>() || expr->is<Luau::AstExprGlobal>())
        return std::nullopt;

    auto* enclosingStmt = findEnclosingStatement(sourceModule, expr);
    if (!enclosingStmt)
        return std::nullopt;

    std::string exprText = textDocument.getText(textDocument.convertLocation(expr->location));
    std::string indent = getLineIndentation(textDocument, enclosingStmt->location.begin.line);

    std::string declaration = indent + "local extracted = " + exprText + "\n";

    size_t insertLine = enclosingStmt->location.begin.line;
    lsp::Range insertRange = {{insertLine, 0}, {insertLine, 0}};
    lsp::Range exprRange = textDocument.convertLocation(expr->location);

    // "extracted" starts after "<indent>local "
    lsp::Position renamePos = {insertLine, indent.size() + 6 /* strlen("local ") */};

    std::vector<lsp::TextEdit> edits;
    edits.push_back({insertRange, declaration});
    edits.push_back({exprRange, "extracted"});

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));
    return RefactoringResult{std::move(workspaceEdit), renamePos};
}

std::optional<RefactoringResult> computeExtractFunctionEdit(
    const lsp::DocumentUri& uri,
    const Luau::SourceModule& sourceModule,
    const TextDocument& textDocument,
    const lsp::Range& range)
{
    auto luauRange = textDocument.convertRange(range);
    auto* block = findEnclosingBlock(sourceModule, luauRange.begin);
    if (!block)
        return std::nullopt;

    auto stmtRange = findStatementsInRange(block, luauRange);
    if (stmtRange.count == 0)
        return std::nullopt;

    // Run free variable analysis on selected statements
    FreeVariableVisitor freeVarVisitor;
    for (size_t i = stmtRange.start; i < stmtRange.start + stmtRange.count; ++i)
        block->body.data[i]->visit(&freeVarVisitor);

    // Reject if selection contains control flow escapes
    if (freeVarVisitor.hasControlFlowEscape)
        return std::nullopt;

    auto freeVars = freeVarVisitor.getFreeVariables();

    // Determine return values: locals defined in the selection that are referenced after it
    std::vector<Luau::AstLocal*> returnVars;
    for (auto* definedLocal : freeVarVisitor.defined)
    {
        auto refs = findSymbolReferences(sourceModule, Luau::Symbol(definedLocal));
        for (const auto& ref : refs)
        {
            // Check if any reference is after the selection
            if (ref.end > block->body.data[stmtRange.start + stmtRange.count - 1]->location.end)
            {
                returnVars.push_back(definedLocal);
                break;
            }
        }
    }

    // Sort return vars by their definition order for stability
    std::sort(returnVars.begin(), returnVars.end(), [](Luau::AstLocal* a, Luau::AstLocal* b)
    {
        return a->location.begin < b->location.begin;
    });

    // Build parameter list
    std::string params;
    for (size_t i = 0; i < freeVars.size(); ++i)
    {
        if (i > 0)
            params += ", ";
        params += freeVars[i]->name.value;
    }

    // Build return list
    std::string returnNames;
    for (size_t i = 0; i < returnVars.size(); ++i)
    {
        if (i > 0)
            returnNames += ", ";
        returnNames += returnVars[i]->name.value;
    }

    // Get the selected text
    auto firstStmt = block->body.data[stmtRange.start];
    auto lastStmt = block->body.data[stmtRange.start + stmtRange.count - 1];
    lsp::Range selectedRange = {
        textDocument.convertPosition(firstStmt->location.begin),
        textDocument.convertPosition(lastStmt->location.end),
    };
    std::string selectedText = textDocument.getText(selectedRange);

    std::string indent = getLineIndentation(textDocument, firstStmt->location.begin.line);
    std::string bodyIndent = indent + "    ";

    // Build the function definition
    // Re-indent the selected text to be inside the function body.
    // Compute the base indentation of the selected code, then replace it with bodyIndent.
    std::string baseIndent = indent; // the indentation of the first selected statement
    std::string functionBody;
    size_t pos = 0;
    while (pos < selectedText.size())
    {
        size_t lineEnd = selectedText.find('\n', pos);
        std::string line;
        if (lineEnd == std::string::npos)
        {
            line = selectedText.substr(pos);
            pos = selectedText.size();
        }
        else
        {
            line = selectedText.substr(pos, lineEnd - pos);
            pos = lineEnd + 1;
        }

        // Strip the base indentation and replace with body indent
        if (line.substr(0, baseIndent.size()) == baseIndent)
            functionBody += bodyIndent + line.substr(baseIndent.size());
        else
            functionBody += bodyIndent + line; // fallback: just prepend body indent

        if (lineEnd != std::string::npos)
            functionBody += "\n";
    }

    if (!returnNames.empty())
    {
        functionBody += "\n" + bodyIndent + "return " + returnNames;
    }

    std::string functionDef = indent + "local function extracted(" + params + ")\n" + functionBody + "\n" + indent + "end\n";

    // Build the call site
    std::string callSite;
    if (!returnVars.empty())
        callSite = indent + "local " + returnNames + " = extracted(" + params + ")";
    else
        callSite = indent + "extracted(" + params + ")";

    // Delete from start of the first statement's line to the end of the last statement
    size_t insertLine = firstStmt->location.begin.line;
    lsp::Range deleteRange = {{insertLine, 0}, {lastStmt->location.end.line + 1, 0}};

    std::string replacement = functionDef + callSite + "\n";

    // "extracted" starts after "<indent>local function "
    lsp::Position renamePos = {insertLine, indent.size() + 15 /* strlen("local function ") */};

    std::vector<lsp::TextEdit> edits;
    edits.push_back({deleteRange, replacement});

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));
    return RefactoringResult{std::move(workspaceEdit), renamePos};
}

std::optional<RefactoringResult> computeInlineVariableEdit(
    const lsp::DocumentUri& uri,
    const Luau::SourceModule& sourceModule,
    const TextDocument& textDocument,
    const lsp::Range& range)
{
    auto luauRange = textDocument.convertRange(range);
    auto exprOrLocal = findExprOrLocalAtPositionClosed(sourceModule, luauRange.begin);

    Luau::AstLocal* local = exprOrLocal.getLocal();
    if (!local)
    {
        if (auto* exprLocal = exprOrLocal.getExpr() ? exprOrLocal.getExpr()->as<Luau::AstExprLocal>() : nullptr)
            local = exprLocal->local;
    }
    if (!local)
        return std::nullopt;

    // Find the declaration
    FindDeclaration finder(local);
    sourceModule.root->visit(&finder);
    if (!finder.result)
        return std::nullopt;

    auto* decl = finder.result;

    // Only support single-variable declarations
    if (decl->vars.size != 1)
        return std::nullopt;

    // Must have an initializer
    if (decl->values.size == 0)
        return std::nullopt;

    auto* initializer = decl->values.data[0];

    // Check that the variable is not reassigned
    IsReassignedVisitor reassignChecker(local);
    sourceModule.root->visit(&reassignChecker);
    if (reassignChecker.reassigned)
        return std::nullopt;

    // Get the initializer text
    std::string initText = textDocument.getText(textDocument.convertLocation(initializer->location));

    bool wrapInParens = needsParentheses(initializer);

    // Find all references
    auto references = findSymbolReferences(sourceModule, Luau::Symbol(local));

    // Build edits: replace each reference with the initializer text
    std::vector<lsp::TextEdit> edits;

    // Sort references in reverse order to avoid offset issues (edits are applied simultaneously by LSP,
    // but we sort for clarity)
    std::vector<Luau::Location> refLocations(references.begin(), references.end());
    std::sort(refLocations.begin(), refLocations.end(), [](const Luau::Location& a, const Luau::Location& b)
    {
        return a.begin > b.begin;
    });

    for (const auto& ref : refLocations)
    {
        // Skip the reference at the declaration site itself
        if (ref == local->location)
            continue;

        lsp::Range refRange = textDocument.convertLocation(ref);
        std::string replacement = wrapInParens ? "(" + initText + ")" : initText;
        edits.push_back({refRange, replacement});
    }

    // Delete the declaration statement (full line)
    lsp::Range deleteRange = {{decl->location.begin.line, 0}, {decl->location.end.line + 1, 0}};
    edits.push_back({deleteRange, ""});

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));
    return RefactoringResult{std::move(workspaceEdit), std::nullopt};
}

// Where a class member's text starts: its attributes, access specifier or `function` keyword, whichever comes first
Luau::Position classMethodStart(const Luau::AstClassMethod& method)
{
    Luau::Position start = method.keywordLocation.begin;

    if (method.qualifierLocation && method.qualifierLocation->begin < start)
        start = method.qualifierLocation->begin;

    for (const Luau::AstAttr* attr : method.function->attributes)
        if (attr->location.begin < start)
            start = attr->location.begin;

    return start;
}

// Luwu Traits (rfcs/classes/traits.md): the class a refactoring can extract into a trait, when `position` is on its `class` keyword or name.
// A trait can't take a class's generic parameters along, and a class with no functions besides `__init` has nothing to
// move, so neither is offered.
Luau::AstStatClass* findExtractableClass(const Luau::SourceModule& sourceModule, const TextDocument& textDocument, const Luau::Position& position)
{
    for (Luau::AstStat* stat : sourceModule.root->body)
    {
        auto* classStat = stat->as<Luau::AstStatClass>();
        if (!classStat || classStat->isTrait)
            continue;

        Luau::Location header{classStat->keywordLocation.begin, classStat->name->location.end};
        if (!header.containsClosed(position))
            continue;

        bool generic = classStat->generics.size > 0 || classStat->genericPacks.size > 0;
        bool hasFunctionToMove = false;
        bool movesWholeLines = true;

        // Functions move by whole lines, so each has to have its lines to itself, strictly inside the class body
        for (const auto& member : classStat->members)
        {
            const auto* method = member.get_if<Luau::AstClassMethod>();
            if (!method || method->functionName == "__init")
                continue;

            hasFunctionToMove = true;

            Luau::Position start = classMethodStart(*method);
            Luau::Position end = method->function->location.end;
            std::string firstLine = textDocument.getLine(start.line);
            std::string lastLine = textDocument.getLine(end.line);

            bool aloneAtStart = firstLine.find_first_not_of(" \t") >= start.column;
            bool aloneAtEnd = lastLine.find_first_not_of(" \t;\r\n", end.column) == std::string::npos;
            bool insideBody = start.line > classStat->name->location.end.line && end.line < classStat->location.end.line;

            if (!aloneAtStart || !aloneAtEnd || !insideBody)
                movesWholeLines = false;
        }

        bool extractable = !generic && hasFunctionToMove && movesWholeLines;
        return extractable ? classStat : nullptr;
    }

    return nullptr;
}

// Luwu Traits (rfcs/classes/traits.md): "Extract class into a trait". The class's functions, except `__init`, move into a new trait
// `<Class>Behavior` written above the class, which expects every field of the class (so the moved code still has them),
// and the class implements the trait. Construction stays with the class. The new trait's name is offered for renaming.
std::optional<RefactoringResult> computeExtractTraitEdit(
    const lsp::DocumentUri& uri,
    const Luau::SourceModule& sourceModule,
    const TextDocument& textDocument,
    const lsp::Range& range)
{
    auto luauRange = textDocument.convertRange(range);
    Luau::AstStatClass* classStat = findExtractableClass(sourceModule, textDocument, luauRange.begin);
    if (!classStat)
        return std::nullopt;

    std::string className = classStat->name->name.value;
    std::string traitName = className + "Behavior";

    // The all-or-nothing access specifier rule carries over: if the class writes them, the trait's expectations must too
    bool qualified = false;
    for (const auto& member : classStat->members)
        if (Luau::visit([](auto&& m) { return m.qualifierLocation.has_value(); }, member))
            qualified = true;
    if (classStat->primaryConstructor)
        for (const auto& qualifiers : classStat->primaryConstructor->argsQualifiers)
            if (qualifiers.qualifierLocation)
                qualified = true;

    auto sourceText = [&](const Luau::Location& location)
    {
        return textDocument.getText(textDocument.convertLocation(location));
    };

    auto expectLine = [&](bool isPrivate, bool isConst, const std::string& name, const Luau::AstType* annotation)
    {
        std::string line = "    expect ";
        if (qualified)
            line += isPrivate ? "private " : "public ";
        if (isConst)
            line += "const ";
        line += name;
        if (annotation)
            line += ": " + sourceText(annotation->location);
        return line + "\n";
    };

    std::string expectations;
    std::vector<std::string> bodyFields;
    for (const auto& member : classStat->members)
    {
        if (const auto* prop = member.get_if<Luau::AstClassProperty>())
        {
            bodyFields.push_back(prop->name.value);
            expectations +=
                expectLine(prop->visibility == Luau::AstClassMemberVisibility::Private, prop->isConst, prop->name.value, prop->ty);
        }
    }

    if (const auto* ctor = classStat->primaryConstructor)
    {
        for (size_t i = 0; i < ctor->args.size; i++)
        {
            const Luau::AstLocal* arg = ctor->args.data[i];
            if (std::find(bodyFields.begin(), bodyFields.end(), arg->name.value) != bodyFields.end())
                continue;

            const Luau::AstClassPrimaryConstructorParamQualifiers* qualifiers =
                i < ctor->argsQualifiers.size ? &ctor->argsQualifiers.data[i] : nullptr;
            bool isPrivate = qualifiers && qualifiers->visibility == Luau::AstClassMemberVisibility::Private;
            bool isConst = qualifiers && qualifiers->isConst;
            expectations += expectLine(isPrivate, isConst, arg->name.value, arg->annotation);
        }
    }

    // Each moved function takes its whole lines along, indentation included: a class body and a trait body indent alike
    std::string functions;
    std::vector<lsp::TextEdit> edits;
    for (const auto& member : classStat->members)
    {
        const auto* method = member.get_if<Luau::AstClassMethod>();
        if (!method || method->functionName == "__init")
            continue;

        Luau::Position start = classMethodStart(*method);
        lsp::Range lines{{start.line, 0}, {method->function->location.end.line + 1, 0}};
        if (!functions.empty())
            functions += "\n";
        functions += textDocument.getText(lines);

        // the blank line that separated the function from what came before it goes too, or the class body is left
        // with a run of them
        lsp::Range removed = lines;
        bool blankAbove = start.line > classStat->name->location.end.line + 1 &&
                          textDocument.getLine(start.line - 1).find_first_not_of(" \t\r\n") == std::string::npos;
        if (blankAbove)
            removed.start.line--;

        edits.push_back(lsp::TextEdit{removed, ""});
    }

    std::string trait = "trait " + traitName + "\n" + expectations;
    if (!expectations.empty())
        trait += "\n";
    trait += functions + "end\n\n";

    lsp::Position classStart{classStat->location.begin.line, 0};
    edits.insert(edits.begin(), lsp::TextEdit{{classStart, classStart}, trait});

    // The class's header gains the trait: at the end of an existing `implements` list, or after the header
    Luau::Position headerEnd = classStat->name->location.end;
    if (classStat->primaryConstructor)
        headerEnd = classStat->primaryConstructor->argLocation.end;

    if (classStat->implements.size > 0)
    {
        Luau::Position listEnd = classStat->implements.data[classStat->implements.size - 1].location.end;
        lsp::Position at = textDocument.convertPosition(listEnd);
        edits.push_back(lsp::TextEdit{{at, at}, ", " + traitName});
    }
    else
    {
        lsp::Position at = textDocument.convertPosition(headerEnd);
        edits.push_back(lsp::TextEdit{{at, at}, " implements " + traitName});
    }

    lsp::WorkspaceEdit workspaceEdit;
    workspaceEdit.changes.emplace(uri, std::move(edits));

    // the trait's name, on the first inserted line, which the edit puts where the class started
    lsp::Position renamePosition{classStat->location.begin.line, 6};
    return RefactoringResult{std::move(workspaceEdit), renamePosition};
}

} // anonymous namespace

void computeRefactorings(
    const lsp::CodeActionParams& params,
    const Luau::SourceModule& sourceModule,
    const TextDocument& textDocument,
    const Luau::Location& requestRange,
    std::vector<lsp::CodeAction>& result)
{
    // Extract Variable: check if selection covers a valid expression
    if (params.context.wants(lsp::CodeActionKind::RefactorExtract))
    {
        auto* expr = findExprCoveringRange(sourceModule, requestRange);
        if (expr && !expr->is<Luau::AstExprLocal>() && !expr->is<Luau::AstExprGlobal>())
        {
            auto* enclosingStmt = findEnclosingStatement(sourceModule, expr);
            if (enclosingStmt)
            {
                lsp::CodeAction action;
                action.title = "Extract to local variable";
                action.kind = lsp::CodeActionKind::RefactorExtract;
                action.data = nlohmann::json{
                    {"uri", params.textDocument.uri.toString()},
                    {"type", "extractVariable"},
                    {"range", params.range},
                };
                result.push_back(std::move(action));
            }
        }

        // Extract Function: check if selection covers complete statements
        auto* block = findEnclosingBlock(sourceModule, requestRange.begin);
        if (block)
        {
            auto stmtRange = findStatementsInRange(block, requestRange);
            if (stmtRange.count > 0)
            {
                // Quick check for control flow escapes
                FreeVariableVisitor checker;
                for (size_t i = stmtRange.start; i < stmtRange.start + stmtRange.count; ++i)
                    block->body.data[i]->visit(&checker);

                if (!checker.hasControlFlowEscape)
                {
                    lsp::CodeAction action;
                    action.title = "Extract to function";
                    action.kind = lsp::CodeActionKind::RefactorExtract;
                    action.data = nlohmann::json{
                        {"uri", params.textDocument.uri.toString()},
                        {"type", "extractFunction"},
                        {"range", params.range},
                    };
                    result.push_back(std::move(action));
                }
            }
        }
    }

    // Luwu Traits (rfcs/classes/traits.md): Extract a class's behavior into a trait it implements
    if (params.context.wants(lsp::CodeActionKind::RefactorExtract))
    {
        if (Luau::AstStatClass* classStat = findExtractableClass(sourceModule, textDocument, requestRange.begin))
        {
            lsp::CodeAction action;
            action.title = "Extract class '" + std::string(classStat->name->name.value) + "' into a trait";
            action.kind = lsp::CodeActionKind::RefactorExtract;
            action.data = nlohmann::json{
                {"uri", params.textDocument.uri.toString()},
                {"type", "extractTrait"},
                {"range", params.range},
            };
            result.push_back(std::move(action));
        }
    }

    // Inline Variable: lightweight check — just verify cursor is on a local.
    // Full validation (single-var decl, has initializer, not reassigned) is deferred to resolve.
    if (params.context.wants(lsp::CodeActionKind::RefactorInline))
    {
        auto exprOrLocal = findExprOrLocalAtPositionClosed(sourceModule, requestRange.begin);

        Luau::AstLocal* local = exprOrLocal.getLocal();
        if (!local)
        {
            if (auto* exprLocal = exprOrLocal.getExpr() ? exprOrLocal.getExpr()->as<Luau::AstExprLocal>() : nullptr)
                local = exprLocal->local;
        }

        if (local)
        {
            lsp::CodeAction action;
            action.title = "Inline variable '" + std::string(local->name.value) + "'";
            action.kind = lsp::CodeActionKind::RefactorInline;
            action.data = nlohmann::json{
                {"uri", params.textDocument.uri.toString()},
                {"type", "inlineVariable"},
                {"range", params.range},
            };
            result.push_back(std::move(action));
        }
    }
}

lsp::CodeAction resolveRefactoring(
    const lsp::CodeAction& action,
    WorkspaceFolder& workspace,
    const LSPCancellationToken& cancellationToken)
{
    lsp::CodeAction resolved = action;

    if (!action.data)
        return resolved;

    auto& data = *action.data;
    auto uri = lsp::DocumentUri::parse(data.at("uri").get<std::string>());
    auto type = data.at("type").get<std::string>();
    auto range = data.at("range").get<lsp::Range>();

    auto moduleName = workspace.fileResolver.getModuleName(uri);
    auto textDocument = workspace.fileResolver.getTextDocument(uri);
    if (!textDocument)
        return resolved;

    Luau::CheckResult cr =
        FFlag::LuauSolverV2 ? workspace.checkStrict(moduleName, cancellationToken, false) : workspace.checkSimple(moduleName, cancellationToken);

    auto sourceModule = workspace.frontend.getSourceModule(moduleName);
    if (!sourceModule)
        return resolved;

    std::optional<RefactoringResult> refactorResult;
    if (type == "extractVariable")
        refactorResult = computeExtractVariableEdit(uri, *sourceModule, *textDocument, range);
    else if (type == "extractFunction")
        refactorResult = computeExtractFunctionEdit(uri, *sourceModule, *textDocument, range);
    else if (type == "inlineVariable")
        refactorResult = computeInlineVariableEdit(uri, *sourceModule, *textDocument, range);
    else if (type == "extractTrait")
        refactorResult = computeExtractTraitEdit(uri, *sourceModule, *textDocument, range);

    if (refactorResult)
    {
        resolved.edit = std::move(refactorResult->edit);

        if (refactorResult->renamePosition)
        {
            auto& pos = *refactorResult->renamePosition;
            resolved.command = lsp::Command{
                "Rename Symbol",
                "luwu.rename",
                {nlohmann::json(uri.toString()), nlohmann::json(pos)},
            };
        }
    }

    return resolved;
}
