#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Luau/Ast.h"
#include "Luau/LinterConfig.h"
#include "LSP/TextDocument.hpp"
#include "Protocol/Structures.hpp"

// Luwu: quick fixes for Luwu's lints. Luwu's linter only reports a code, a location and a message, so each fix finds
// the node the warning is on again and builds the edit from the source text. Every lint with a fix here also gets an
// action fixing every warning of it in the module.
namespace Luau::LanguageServer::LintFixes
{

struct LintFix
{
    std::string title;
    std::vector<lsp::TextEdit> edits;
    // Whether an editor may apply it on its own (VS Code's "auto fix"). False when the fix might not be what was meant.
    bool preferred = true;
};

// The fixes for one warning, best first, if its lint has any and the code it's on can be rewritten
std::vector<LintFix> computeLintFixes(const Luau::LintWarning& lint, Luau::AstStatBlock* root, const TextDocument& textDocument);

// The title of the action that fixes every warning of `code` in a module, or nothing when the lint has no fix here
std::optional<std::string> fixAllTitle(Luau::LintWarning::Code code);

// The edits applying the first fix of every warning of `code` in `warnings`. A fix whose edits overlap an earlier one's
// is left out, and the next run of the action picks it up.
std::vector<lsp::TextEdit> computeFixAllEdits(
    Luau::LintWarning::Code code, const std::vector<Luau::LintWarning>& warnings, Luau::AstStatBlock* root, const TextDocument& textDocument);

} // namespace Luau::LanguageServer::LintFixes
