#pragma once
#include "Luau/Location.h"
#include "Luau/Module.h"
#include "Luau/Frontend.h"
#include "Protocol/SemanticTokens.hpp"
#include "LSP/TextDocument.hpp"

struct SemanticToken
{
    Luau::Position start;
    Luau::Position end;
    lsp::SemanticTokenTypes tokenType;
    lsp::SemanticTokenModifiers tokenModifiers;
};

/// `grammarHighlightsNone`: the document is a Luwu file, whose grammar already scopes `none` as a
/// language constant, so it gets no semantic token. `textDocument` is the source, which a read the parser made up is
/// checked against: without it, those reads get no token.
std::vector<SemanticToken> getSemanticTokens(const Luau::Frontend& frontend, const Luau::ModulePtr& module, const Luau::SourceModule* sourceModule,
    bool grammarHighlightsNone = false, const TextDocument* textDocument = nullptr);

/// Luwu: Moonwave tags starting doc comment lines, and the names `@param`/`@field`/`@prop` give (Luau files only; the
/// caller decides).
void addMoonwaveTagTokens(std::vector<SemanticToken>& tokens, const Luau::SourceModule* sourceModule, const TextDocument* textDocument);
