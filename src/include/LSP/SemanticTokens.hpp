#pragma once
#include "Luau/Location.h"
#include "Luau/Module.h"
#include "Luau/Frontend.h"
#include "Protocol/SemanticTokens.hpp"

struct SemanticToken
{
    Luau::Position start;
    Luau::Position end;
    lsp::SemanticTokenTypes tokenType;
    lsp::SemanticTokenModifiers tokenModifiers;
};

/// `grammarHighlightsNone`: the document is a Luwu file, whose grammar already scopes `none` as a
/// language constant, so it gets no semantic token.
std::vector<SemanticToken> getSemanticTokens(const Luau::Frontend& frontend, const Luau::ModulePtr& module, const Luau::SourceModule* sourceModule,
    bool grammarHighlightsNone = false);
