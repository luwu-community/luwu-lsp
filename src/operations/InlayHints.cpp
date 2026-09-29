#include "LSP/DocumentationParser.hpp"
#include "LSP/Workspace.hpp"

#include <algorithm>

#include "Luau/Ast.h"
#include "Luau/AstQuery.h"
#include "Luau/ToString.h"
#include "Luau/PrettyPrinter.h"
#include "LSP/LuauExt.hpp"

static std::vector<lsp::InlayHintLabelPart> toInlayHintLabelParts(
    const Luau::ToStringResult& result, const Client* client, WorkspaceFileResolver* fileResolver)
{
    std::vector<lsp::InlayHintLabelPart> parts;

    if (result.typeSpans.empty())
    {
        parts.push_back(lsp::InlayHintLabelPart{result.name});
        return parts;
    }

    auto spans = result.typeSpans;
    std::sort(spans.begin(), spans.end(),
        [](const Luau::ToStringSpan& a, const Luau::ToStringSpan& b)
        {
            return a.startPos < b.startPos;
        });

    size_t nameLen = result.name.length();
    size_t lastEnd = 0;
    for (const auto& [start, end, typeId] : spans)
    {
        if (start >= nameLen)
            break;

        size_t clampedEnd = std::min(end, nameLen);

        if (start > lastEnd)
            parts.emplace_back(lsp::InlayHintLabelPart{result.name.substr(lastEnd, start - lastEnd)});

        lsp::InlayHintLabelPart part;
        part.value = result.name.substr(start, clampedEnd - start);
        part.location = types::getTypeLocation(typeId, fileResolver);
        if (typeId->documentationSymbol)
        {
            if (auto documentation = printDocumentation(client->documentation, *typeId->documentationSymbol))
                part.tooltip = lsp::MarkupContent{lsp::MarkupKind::Markdown, *documentation};
        }
        parts.push_back(part);

        lastEnd = clampedEnd;
    }

    if (lastEnd < nameLen)
        parts.emplace_back(lsp::InlayHintLabelPart{result.name.substr(lastEnd)});

    return parts;
}

// Pretty-prints an expression onto a single line, truncating it if it's too long to be a useful
// hint (e.g. a large inline table or function passed as the iterator expression).
std::string singleLineExprString(Luau::AstExpr* expr, size_t maxLen = 40)
{
    // Luau::toString() has no length limit and fully pretty-prints the entire subtree before we
    // get a chance to truncate it -- for a large inline table/call chain that can mean allocating
    // megabytes (or crashing) just to build a hint we'd immediately cut down to `maxLen` chars.
    // Bail out early using only cheap Location arithmetic, before ever calling it, whenever the
    // expression couldn't possibly fit anyway.
    const auto& begin = expr->location.begin;
    const auto& end = expr->location.end;
    if (end.line != begin.line || end.column < begin.column || end.column - begin.column > maxLen * 4)
        return "...";

    std::string result = Luau::toString(expr);
    std::replace(result.begin(), result.end(), '\n', ' ');

    // Collapse repeated whitespace left over from multi-line formatting
    std::string collapsed;
    bool lastWasSpace = false;
    for (char c : result)
    {
        bool isSpace = std::isspace(static_cast<unsigned char>(c));
        if (isSpace && lastWasSpace)
            continue;
        collapsed += isSpace ? ' ' : c;
        lastWasSpace = isSpace;
    }

    if (collapsed.size() > maxLen)
        collapsed = collapsed.substr(0, maxLen) + "...";

    return collapsed;
}

bool isLiteral(const Luau::AstExpr* expr)
{
    return expr->is<Luau::AstExprConstantBool>() || expr->is<Luau::AstExprConstantString>() || expr->is<Luau::AstExprConstantNumber>() ||
           expr->is<Luau::AstExprConstantNil>();
}

// Function with no statements in body
bool isNoOpFunction(const Luau::AstExprFunction* func)
{
    return func->body->body.size == 0;
}

// Adds a text edit onto the hint so that it can be inserted.
void makeInsertable(const ClientConfiguration& config, lsp::InlayHint& hint, Luau::TypeId ty)
{
    if (!config.inlayHints.makeInsertable)
        return;

    Luau::ToStringOptions opts;
    auto result = Luau::toStringDetailed(ty, opts);
    if (result.invalid || result.truncated || result.error || result.cycle)
        return;
    hint.textEdits.emplace_back(lsp::TextEdit{{hint.position, hint.position}, ": " + result.name});
}

void makeInsertable(const ClientConfiguration& config, lsp::InlayHint& hint, Luau::TypePackId ty, bool removeLeadingEllipsis = false)
{
    if (!config.inlayHints.makeInsertable)
        return;

    auto result = types::toStringReturnTypeDetailed(ty);
    if (result.invalid || result.truncated || result.error || result.cycle)
        return;
    auto name = result.name;
    if (removeLeadingEllipsis)
        name = removePrefix(name, "...");
    hint.textEdits.emplace_back(lsp::TextEdit{{hint.position, hint.position}, ": " + name});
}
struct InlayHintVisitor : public Luau::AstVisitor
{
    const Luau::ModulePtr& module;
    const ClientConfiguration& config;
    const Client* client;
    const TextDocument* textDocument;
    WorkspaceFileResolver* fileResolver;
    std::vector<lsp::InlayHint> hints{};
    Luau::ToStringOptions stringOptions;

    explicit InlayHintVisitor(const Luau::ModulePtr& module, const ClientConfiguration& config, const Client* client,
        const TextDocument* textDocument, WorkspaceFileResolver* fileResolver)
        : module(module)
        , config(config)
        , client(client)
        , textDocument(textDocument)
        , fileResolver(fileResolver)

    {
        stringOptions.maxTableLength = 30;
        stringOptions.maxTypeLength = config.inlayHints.typeHintMaxLength;
    }

    void setLabelFromType(lsp::InlayHint& hint, Luau::TypeId ty, const std::string& prefix = ": ")
    {
        auto result = Luau::toStringDetailed(ty, stringOptions);
        hint.label.push_back(lsp::InlayHintLabelPart{prefix});
        auto parts = toInlayHintLabelParts(result, client, fileResolver);
        hint.label.insert(hint.label.end(), parts.begin(), parts.end());
    }

    void setLabelFromTypePack(lsp::InlayHint& hint, Luau::TypePackId ty, const std::string& prefix = ": ", bool removeEllipsis = false)
    {
        auto result = types::toStringReturnTypeDetailed(ty, stringOptions);
        if (removeEllipsis)
            result.name = removePrefix(result.name, "...");
        hint.label.push_back(lsp::InlayHintLabelPart{prefix});
        auto parts = toInlayHintLabelParts(result, client, fileResolver);
        hint.label.insert(hint.label.end(), parts.begin(), parts.end());
    }

    // Adds a trailing hint after the `end` of a block that spans at least `blockEndHintsMinLines`
    // lines, naming what it closes (e.g. "function foo") -- similar to rust-analyzer's closing
    // brace hints, useful for finding your way back after scrolling past a long block. No comment
    // marker is prepended since inlay hints already render as non-insertable ghost text.
    void addBlockEndHint(const Luau::Location& location, const std::string& label)
    {
        if (!config.inlayHints.blockEndHints)
            return;

        if (location.end.line <= location.begin.line)
            return;

        size_t lineSpan = location.end.line - location.begin.line + 1;
        if (lineSpan < config.inlayHints.blockEndHintsMinLines)
            return;

        lsp::InlayHint hint;
        hint.position = textDocument->convertPosition(location.end);
        hint.paddingLeft = true;
        hint.label.push_back(lsp::InlayHintLabelPart{" " + label + " "});
        hints.emplace_back(hint);
    }

    bool visit(Luau::AstStatLocal* local) override
    {
        if (!config.inlayHints.variableTypes)
            return true;

        auto scope = Luau::findScopeAtPosition(*module, local->location.begin);
        if (!scope)
            return false;

        for (size_t i = 0; i < local->vars.size; i++)
        {
            auto var = local->vars.data[i];
            if (!var->annotation)
            {
                auto ty = scope->lookup(var);
                if (ty)
                {
                    auto followedTy = Luau::follow(*ty);

                    // If the variable is assigned a function, don't bother showing a hint
                    // since we can already infer stuff from the assigned function
                    if (local->values.size > i)
                    {
                        if (Luau::get<Luau::FunctionType>(followedTy) && local->values.data[i]->is<Luau::AstExprFunction>())
                            continue;
                    }

                    // If the variable is named "_", don't include an inlay hint
                    if (var->name == "_")
                        continue;

                    if (config.inlayHints.hideHintsForErrorTypes && Luau::get<Luau::ErrorType>(followedTy))
                        continue;

                    auto typeString = Luau::toString(followedTy, stringOptions);

                    // If the stringified type is equivalent to the variable name, don't bother
                    // showing an inlay hint
                    if (Luau::equalsLower(typeString, var->name.value))
                        continue;

                    lsp::InlayHint hint;
                    hint.kind = lsp::InlayHintKind::Type;
                    hint.position = textDocument->convertPosition(var->location.end);
                    setLabelFromType(hint, followedTy);
                    makeInsertable(config, hint, followedTy);
                    hints.emplace_back(hint);
                }
            }
        }

        return true;
    }

    bool visit(Luau::AstStatForIn* forIn) override
    {
        {
            std::string varNames;
            for (size_t i = 0; i < forIn->vars.size; i++)
            {
                if (i > 0)
                    varNames += ", ";
                varNames += forIn->vars.data[i]->name.value;
            }

            std::string valueNames;
            for (size_t i = 0; i < forIn->values.size; i++)
            {
                if (i > 0)
                    valueNames += ", ";
                valueNames += singleLineExprString(forIn->values.data[i]);
            }

            addBlockEndHint(forIn->location, "for " + varNames + " in " + valueNames);
        }

        if (!config.inlayHints.variableTypes)
            return true;

        auto scope = Luau::findScopeAtPosition(*module, forIn->location.begin);
        if (!scope)
            return false;

        for (size_t i = 0; i < forIn->vars.size; i++)
        {
            auto var = forIn->vars.data[i];
            if (!var->annotation)
            {
                auto ty = scope->lookup(var);
                if (ty)
                {
                    auto followedTy = Luau::follow(*ty);

                    // If the variable is named "_", don't include an inlay hint
                    if (var->name == "_")
                        continue;

                    if (config.inlayHints.hideHintsForErrorTypes && Luau::get<Luau::ErrorType>(followedTy))
                        continue;

                    auto typeString = Luau::toString(followedTy, stringOptions);

                    // If the stringified type is equivalent to the variable name, don't bother
                    // showing an inlay hint
                    if (Luau::equalsLower(typeString, var->name.value))
                        continue;

                    lsp::InlayHint hint;
                    hint.kind = lsp::InlayHintKind::Type;
                    hint.position = textDocument->convertPosition(var->location.end);
                    setLabelFromType(hint, followedTy);
                    makeInsertable(config, hint, followedTy);
                    hints.emplace_back(hint);
                }
            }
        }

        return true;
    }

    bool visit(Luau::AstStatFor* for_) override
    {
        addBlockEndHint(for_->location, "for " + std::string(for_->var->name.value));
        return true;
    }

    bool visit(Luau::AstStatWhile* while_) override
    {
        addBlockEndHint(while_->location, "while");
        return true;
    }

    bool visit(Luau::AstStatIf* ifStat) override
    {
        addBlockEndHint(ifStat->location, "if " + singleLineExprString(ifStat->condition));

        // Every `elseif` in a chain is its own nested AstStatIf (as `elsebody`), sharing the same
        // `location.end` (the chain's final `end`) as the outer `if` -- so letting the default
        // recursion re-enter this override for each of them would stack up duplicate hints at that
        // same position. Walk the chain manually instead: visit each condition/thenbody ourselves,
        // and only recurse normally (via `visit`) into a trailing `else` block.
        Luau::AstStat* current = ifStat;
        while (auto* asIf = current->as<Luau::AstStatIf>())
        {
            asIf->condition->visit(this);
            asIf->thenbody->visit(this);

            if (!asIf->elsebody)
                break;

            if (asIf->elsebody->as<Luau::AstStatIf>())
            {
                current = asIf->elsebody;
                continue;
            }

            asIf->elsebody->visit(this);
            break;
        }

        return false;
    }

    bool visit(Luau::AstStatClass* classStat) override
    {
        // Luwu Traits (rfcs/classes/traits.md): a trait is parsed as a class
        addBlockEndHint(classStat->location, (classStat->isTrait ? "trait " : "class ") + std::string(classStat->name->name.value));
        return true;
    }

    bool visit(Luau::AstExprFunction* func) override
    {
        if (func->debugname.value && *func->debugname.value)
            addBlockEndHint(func->location, "function " + std::string(func->debugname.value));

        auto ty = module->astTypes.find(func);
        if (!ty)
            return false;

        auto followedTy = Luau::follow(*ty);
        if (auto ftv = Luau::get<Luau::FunctionType>(followedTy))
        {
            // Add return type annotation
            if (config.inlayHints.functionReturnTypes)
            {
                if (!func->returnAnnotation && func->argLocation && !isNoOpFunction(func))
                {
                    lsp::InlayHint hint;
                    hint.kind = lsp::InlayHintKind::Type;
                    hint.position = textDocument->convertPosition(func->argLocation->end);
                    setLabelFromTypePack(hint, ftv->retTypes);
                    makeInsertable(config, hint, ftv->retTypes);
                    hints.emplace_back(hint);
                }
            }

            // Parameter types hint
            if (config.inlayHints.parameterTypes)
            {
                auto it = Luau::begin(ftv->argTypes);
                if (it != Luau::end(ftv->argTypes))
                {
                    // Skip first item if it is self
                    if (func->self && isMethod(ftv))
                        it++;

                    for (auto param : func->args)
                    {
                        if (it == Luau::end(ftv->argTypes))
                            break;

                        auto argType = *it;
                        if (!param->annotation && param->name != "_")
                        {
                            lsp::InlayHint hint;
                            hint.kind = lsp::InlayHintKind::Type;
                            hint.position = textDocument->convertPosition(param->location.end);
                            setLabelFromType(hint, argType);
                            makeInsertable(config, hint, argType);
                            hints.emplace_back(hint);
                        }

                        it++;
                    }
                }

                if (func->vararg && it.tail())
                {
                    auto varargType = *it.tail();
                    if (!func->varargAnnotation)
                    {
                        lsp::InlayHint hint;
                        hint.kind = lsp::InlayHintKind::Type;
                        hint.position = textDocument->convertPosition(func->varargLocation.end);
                        setLabelFromTypePack(hint, varargType, ": ", /* removeEllipsis: */ true);
                        makeInsertable(config, hint, varargType, /* removeLeadingEllipsis: */ true);
                        hints.emplace_back(hint);
                    }
                }
            }
        }

        return true;
    }

    bool visit(Luau::AstExprCall* call) override
    {
        if (config.inlayHints.parameterNames == InlayHintsParameterNamesConfig::None)
            return true;

        auto ty = module->astTypes.find(call->func);
        if (!ty)
            return false;

        auto followedTy = Luau::follow(*ty);
        if (auto ftv = Luau::get<Luau::FunctionType>(followedTy))
        {
            if (ftv->argNames.size() == 0)
                return true;

            auto namesIt = ftv->argNames.begin();
            auto idx = 0;
            for (auto param : call->args)
            {
                // Skip first item if method call (`:`)
                if (idx == 0 && call->self)
                    namesIt++;

                if (namesIt == ftv->argNames.end())
                    break;

                if (!namesIt->has_value())
                {
                    namesIt++;
                    idx++;
                    continue;
                }

                auto createHint = true;
                auto paramName = (*namesIt)->name;
                if (!isLiteral(param))
                {
                    if (config.inlayHints.parameterNames == InlayHintsParameterNamesConfig::Literals)
                        createHint = false;

                    // If the name somewhat matches the arg name, we can skip the inlay hint
                    std::string stringifiedParam = Luau::toString(param);
                    if (auto indexName = param->as<Luau::AstExprIndexName>())
                        stringifiedParam = Luau::toString(indexName->index);
                    if (config.inlayHints.hideHintsForMatchingParameterNames && Luau::equalsLower(stringifiedParam, paramName))
                        createHint = false;
                }

                // Ignore the parameter name if its just "_"
                if (paramName == "_")
                    createHint = false;

                // TODO: only apply in specific situations
                if (createHint)
                {
                    lsp::InlayHint hint;
                    hint.kind = lsp::InlayHintKind::Parameter;
                    hint.label.push_back(lsp::InlayHintLabelPart{paramName + ":"});
                    hint.position = textDocument->convertPosition(param->location.begin);
                    hint.paddingRight = true;
                    hints.emplace_back(hint);
                }

                namesIt++;
                idx++;
            }
        }

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

lsp::InlayHintResult WorkspaceFolder::inlayHint(const lsp::InlayHintParams& params, const LSPCancellationToken& cancellationToken)
{
    auto config = client->getConfiguration(rootUri);

    auto moduleName = fileResolver.getModuleName(params.textDocument.uri);
    auto textDocument = fileResolver.getTextDocument(params.textDocument.uri);
    if (!textDocument)
        throw JsonRpcException(lsp::ErrorCode::RequestFailed, "No managed text document for " + params.textDocument.uri.toString());

    // TODO: expressiveTypes - remove "forAutocomplete" once the types have been fixed
    checkStrict(moduleName, cancellationToken, /* forAutocomplete: */ config.hover.strictDatamodelTypes);
    throwIfCancelled(cancellationToken);

    auto sourceModule = frontend.getSourceModule(moduleName);
    auto module = getModule(moduleName, /* forAutocomplete: */ config.hover.strictDatamodelTypes);
    if (!sourceModule || !module)
        return {};

    InlayHintVisitor visitor{module, config, client, textDocument, &fileResolver};
    visitor.visit(sourceModule->root);

    return visitor.hints;
}
