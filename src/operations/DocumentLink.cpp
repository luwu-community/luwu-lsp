#include "LSP/Workspace.hpp"
#include "LSP/LanguageServer.hpp"
#include "LSP/LuauExt.hpp"

struct RequireInfo
{
    Luau::AstExpr* require = nullptr;
    Luau::Location location;
};

struct FindRequireVisitor : public Luau::AstVisitor
{
    std::vector<RequireInfo> requireInfos{};

    bool visit(Luau::AstExprCall* call) override
    {
        if (auto maybeRequire = types::matchRequire(*call))
            requireInfos.emplace_back(RequireInfo{*maybeRequire, call->argLocation});
        return true;
    }

    bool visit(Luau::AstStatBlock* block) override
    {
        for (Luau::AstStat* stat : block->body)
        {
            stat->visit(this);
        }

        return false;
    }
};

std::optional<lsp::Location> WorkspaceFolder::docRefTargetLocation(const Luau::DocRefTarget& target)
{
    // a type from a definitions file (`@seal`) names its package, which the file resolver doesn't know, but whose
    // document is kept loaded
    if (auto definitionsFile = definitionsFileState.find(target.module); definitionsFile != definitionsFileState.end())
    {
        const TextDocument& document = definitionsFile->second.textDocument;
        return lsp::Location{document.uri(), {document.convertPosition(target.definition.begin), document.convertPosition(target.definition.end)}};
    }

    auto document = fileResolver.getOrCreateTextDocumentFromModuleName(target.module);
    if (!document)
        return std::nullopt;
    return lsp::Location{document->uri(), {document->convertPosition(target.definition.begin), document->convertPosition(target.definition.end)}};
}

std::optional<Uri> WorkspaceFolder::docRefTargetLink(const Luau::DocRefTarget& target)
{
    auto location = docRefTargetLocation(target);
    if (!location)
        return std::nullopt;
    // VS Code opens `#L<line>,<column>` (1-based) at that position
    const Uri& uri = location->uri;
    return Uri(uri.scheme, uri.authority, uri.path, uri.query,
        "L" + std::to_string(location->range.start.line + 1) + "," + std::to_string(location->range.start.character + 1));
}

std::vector<lsp::DocumentLink> WorkspaceFolder::documentLink(const lsp::DocumentLinkParams& params)
{
    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    std::vector<lsp::DocumentLink> result{};

    frontend.parse(moduleName);

    auto sourceModule = frontend.getSourceModule(moduleName);
    if (!sourceModule || !sourceModule->root)
        return {};

    FindRequireVisitor visitor;
    visitor.visit(sourceModule->root);

    for (auto& require : visitor.requireInfos)
    {
        if (auto moduleInfo = frontend.moduleResolver.resolveModuleInfo(moduleName, *require.require))
        {
            // Resolve the module info to a URI
            if (auto uri = platform->resolveToRealPath(moduleInfo->name))
            {
                lsp::DocumentLink link;
                link.target = *uri;
                Luau::Position endPos{require.location.end.line, require.location.end.column - 1};
                link.range = {textDocument->convertPosition(require.location.begin), textDocument->convertPosition(endPos)};
                result.push_back(link);
            }
        }
    }

    // Luwu doc references: each name in a doc comment's reference links to where it's declared (`fs`, `file` and `read` in
    // `[fs.file.read]` separately). Needs the module checked, which the diagnostics after an edit do.
    if (auto module = getModule(moduleName))
    {
        for (const Luau::DocRefTarget& target : module->docRefTargets)
        {
            if (auto link = docRefTargetLink(target))
            {
                lsp::DocumentLink documentLink;
                documentLink.target = *link;
                documentLink.range = {textDocument->convertPosition(target.location.begin), textDocument->convertPosition(target.location.end)};
                result.push_back(documentLink);
            }
        }
    }

    return result;
}

std::vector<lsp::DocumentLink> LanguageServer::documentLink(const lsp::DocumentLinkParams& params)
{
    auto workspace = findWorkspace(params.textDocument.uri);
    return workspace->documentLink(params);
}
