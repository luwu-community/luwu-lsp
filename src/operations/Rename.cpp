#include "LSP/Workspace.hpp"

#include "Luau/AstQuery.h"
#include "LSP/LuauExt.hpp"

static std::optional<Luau::Binding> getBinding(
    const WorkspaceFolder* workspaceFolder, const Luau::ModuleName& moduleName, const Luau::Position& position)
{
    auto sourceModule = workspaceFolder->frontend.getSourceModule(moduleName);
    // TODO: fix "forAutocomplete"
    auto module = workspaceFolder->getModule(moduleName, /* forAutocomplete: */ true);
    auto binding = Luau::findBindingAtPosition(*module, *sourceModule, position);
    return binding;
}

static bool isGlobalBinding(const Luau::Binding& binding)
{
    return binding.location.begin == Luau::Position{0, 0} && binding.location.end == Luau::Position{0, 0};
}

static std::vector<lsp::Location> toLspLocations(
    const lsp::DocumentUri& uri, const TextDocument& textDocument, const std::vector<Luau::Location>& locations)
{
    std::vector<lsp::Location> result;
    result.reserve(locations.size());
    for (const auto& location : locations)
        result.emplace_back(
            lsp::Location{uri, {textDocument.convertPosition(location.begin), textDocument.convertPosition(location.end)}});
    return result;
}

static std::vector<lsp::Location> classMemberLocations(
    WorkspaceFolder* workspaceFolder, const types::ClassMemberOrigin& origin, const Luau::Name& name, const LSPCancellationToken& cancellationToken)
{
    std::vector<lsp::Location> result;
    for (const auto& reference : workspaceFolder->findAllClassMemberReferences(origin, name, cancellationToken))
        if (auto textDocument = workspaceFolder->fileResolver.getOrCreateTextDocumentFromModuleName(reference.moduleName))
            result.emplace_back(lsp::Location{textDocument->uri(),
                {textDocument->convertPosition(reference.location.begin), textDocument->convertPosition(reference.location.end)}});
    return result;
}

// Finds the class statement (always top-level) in `root` whose own name is `local`, if any.
static Luau::AstStatClass* findClassStatByNameLocal(Luau::AstStatBlock* root, Luau::AstLocal* local)
{
    for (Luau::AstStat* stat : root->body)
        if (auto* classStat = stat->as<Luau::AstStatClass>())
            if (classStat->name == local)
                return classStat;
    return nullptr;
}

// Finds the class statement (always top-level) in `root` named `name`. Class names aren't pushed as
// locals, so value usages of a class (`Dog(...)`, `Dog.staticFn`) parse as globals with its name.
static Luau::AstStatClass* findClassStatByName(Luau::AstStatBlock* root, const Luau::AstName& name)
{
    for (Luau::AstStat* stat : root->body)
        if (auto* classStat = stat->as<Luau::AstStatClass>())
            if (classStat->name->name == name)
                return classStat;
    return nullptr;
}

static std::vector<lsp::Location> findAllReferencesForRenaming(
    WorkspaceFolder* workspaceFolder, const lsp::RenameParams& params, const LSPCancellationToken& cancellationToken)
{
    lsp::ReferenceParams referenceParams{};
    referenceParams.textDocument = params.textDocument;
    referenceParams.position = params.position;
    referenceParams.context.includeDeclaration = true;
    return workspaceFolder->references(referenceParams, cancellationToken).value_or(std::vector<lsp::Location>{});
}

std::vector<lsp::Location> getReferencesForRenaming(
    WorkspaceFolder* workspaceFolder, const lsp::RenameParams& params, const LSPCancellationToken& cancellationToken)
{
    auto moduleName = workspaceFolder->fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = workspaceFolder->fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());
    auto position = textDocument->convertPosition(params.position);
    workspaceFolder->checkStrict(moduleName, cancellationToken);
    throwIfCancelled(cancellationToken);

    auto sourceModule = workspaceFolder->frontend.getSourceModule(moduleName);
    if (!sourceModule)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "Unable to read source code");

    // Renaming a class name, from either its declaration (`class Dog`) or a usage (constructor
    // call, static/method access via `Dog.member`, or a `: Dog` type annotation).
    if (auto* classStat = types::findClassStatContainingPosition(sourceModule->root, position);
        classStat && classStat->name->location.containsClosed(position))
        return toLspLocations(params.textDocument.uri, *textDocument, types::findClassNameReferences(*sourceModule, classStat));

    // Renaming a field, method, or static function, from its declaration inside a class or trait. An expected
    // function's signature spans its name, so this can't skip positions inside methods.
    if (auto* classStat = types::findEnclosingClassStat(sourceModule->root, position))
    {
        for (const auto& member : classStat->members)
        {
            if (const auto* prop = member.get_if<Luau::AstClassProperty>(); prop && prop->nameLocation.containsClosed(position))
                return classMemberLocations(workspaceFolder, {moduleName, prop->nameLocation}, prop->name.value, cancellationToken);
            else if (const auto* method = member.get_if<Luau::AstClassMethod>(); method && method->nameLocation.containsClosed(position))
                return classMemberLocations(workspaceFolder, {moduleName, method->nameLocation}, method->functionName.value, cancellationToken);
        }
    }

    // Find All References can return cross module references of a symbol
    // If this is a local symbol in a file, then just rename that instead
    auto exprOrLocal = findExprOrLocalAtPositionClosed(*sourceModule, position);

    // Renaming a class from a value usage (`Dog(...)`, `Dog.staticFn`), which parses as a global.
    if (auto expr = exprOrLocal.getExpr())
    {
        if (auto global = expr->as<Luau::AstExprGlobal>())
        {
            if (auto* classStat = findClassStatByName(sourceModule->root, global->name))
                return toLspLocations(params.textDocument.uri, *textDocument, types::findClassNameReferences(*sourceModule, classStat));
        }
    }

    // Renaming a class from a type annotation usage (`local x: Dog`).
    if (auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position, /* includeTypes= */ true); !ancestry.empty())
    {
        if (auto reference = ancestry.back()->as<Luau::AstTypeReference>();
            reference && !reference->prefix && reference->nameLocation.containsClosed(position))
        {
            if (auto* classStat = findClassStatByName(sourceModule->root, reference->name))
                return toLspLocations(params.textDocument.uri, *textDocument, types::findClassNameReferences(*sourceModule, classStat));
        }
    }

    // Renaming a type function, from its declaration or a call from another type function's body (which parses as a
    // global). Its type usages (`Name<T>`) fall through to find all references below.
    if (findTypeFunctionAtPosition(*sourceModule, position))
        return findAllReferencesForRenaming(workspaceFolder, params, cancellationToken);

    if (auto binding = getBinding(workspaceFolder, moduleName, position); binding && isGlobalBinding(*binding))
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "Cannot rename a global variable");

    Luau::Symbol symbol;
    if (exprOrLocal.getLocal())
        symbol = exprOrLocal.getLocal();
    else if (auto expr = exprOrLocal.getExpr())
    {
        if (auto local = expr->as<Luau::AstExprLocal>())
            symbol = local->local;
        else if (auto global = expr->as<Luau::AstExprGlobal>())
            symbol = global->name;
        else if (auto indexName = expr->as<Luau::AstExprIndexName>())
        {
            // Renaming a field, method, or static function from a usage site (`dog.name`/`Dog.method`/`dog:method`),
            // wherever the class or trait declaring it lives.
            auto module = workspaceFolder->getModule(moduleName, /* forAutocomplete: */ true);
            if (auto ty = module->astTypes.find(indexName->expr))
            {
                if (auto origin = types::findClassMemberOrigin(*ty, indexName->index.value))
                {
                    auto locations = classMemberLocations(workspaceFolder, *origin, indexName->index.value, cancellationToken);
                    if (locations.empty())
                        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "Cannot rename a member declared in a definition file");
                    return locations;
                }
            }
        }
    }

    if (symbol)
    {
        // If this local is a class's own name, include type-annotation usages too.
        if (symbol.local)
        {
            if (auto* classStat = findClassStatByNameLocal(sourceModule->root, symbol.local))
                return toLspLocations(params.textDocument.uri, *textDocument, types::findClassNameReferences(*sourceModule, classStat));
        }

        auto references = findSymbolReferences(*sourceModule, symbol);
        return toLspLocations(params.textDocument.uri, *textDocument, references);
    }
    else
        return findAllReferencesForRenaming(workspaceFolder, params, cancellationToken);
}

/// Checks if the given location refers to a quoted string literal that needs range adjustment during rename.
/// If so, returns an adjusted range that excludes the quotes (so the edit replaces only the content inside).
/// For table keys, only adjusts if it's bracket notation (General kind), not shorthand syntax.
static std::optional<lsp::Range> getAdjustedRangeIfQuotedString(WorkspaceFolder* workspaceFolder, const lsp::Location& location)
{
    auto moduleName = workspaceFolder->fileResolver.getModuleName(location.uri);
    auto sourceModule = workspaceFolder->frontend.getSourceModule(moduleName);
    auto textDocument = workspaceFolder->fileResolver.getOrCreateTextDocumentFromModuleName(moduleName);
    if (!sourceModule || !textDocument)
        return std::nullopt;

    auto position = textDocument->convertPosition(location.range.start);

    auto ancestry = Luau::findAstAncestryOfPosition(*sourceModule, position, /* includeTypes= */ false);
    if (ancestry.size() < 2)
        return std::nullopt;

    auto node = ancestry.back();
    auto constantString = node->as<Luau::AstExprConstantString>();
    if (!constantString)
        return std::nullopt;

    // Check if this string is a table key - if so, we need to verify it's bracket notation (General kind)
    auto parent = ancestry.at(ancestry.size() - 2);
    if (auto table = parent->as<Luau::AstExprTable>())
    {
        for (const auto& item : table->items)
        {
            if (item.key == node)
            {
                if (item.kind != Luau::AstExprTable::Item::Kind::General)
                    return std::nullopt;
                break;
            }
        }
    }

    return lsp::Range{{location.range.start.line, location.range.start.character + 1}, {location.range.end.line, location.range.end.character - 1}};
}

lsp::RenameResult WorkspaceFolder::rename(const lsp::RenameParams& params, const LSPCancellationToken& cancellationToken)
{
    // Verify the new name is valid (is an identifier)
    if (params.newName.length() == 0)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "The new name must be a valid identifier");
    if (!isalpha(params.newName.at(0)) && params.newName.at(0) != '_')
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "The new name must be a valid identifier starting with a character or underscore");

    if (!Luau::isIdentifier(params.newName))
        throw JsonRpcException(
            lsp::ErrorCode::RequestFailed, "The new name must be a valid identifier composed of characters, digits, and underscores only");

    auto references = getReferencesForRenaming(this, params, cancellationToken);
    if (references.empty())
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "Unable to find symbol to rename");

    lsp::WorkspaceEdit result{};

    for (const auto& reference : references)
    {
        if (result.changes.find(reference.uri) == result.changes.end())
            result.changes.insert_or_assign(reference.uri, std::vector<lsp::TextEdit>{});

        // For quoted strings (bracket notation), adjust range to exclude quotes
        lsp::Range editRange = reference.range;
        if (auto adjustedRange = getAdjustedRangeIfQuotedString(this, reference))
            editRange = *adjustedRange;

        result.changes.at(reference.uri).emplace_back(lsp::TextEdit{editRange, params.newName});
    }

    return result;
}
