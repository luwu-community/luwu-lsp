#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Luau/Ast.h"
#include "Luau/Location.h"
#include "Luau/ParseResult.h"

/// Returns markdown documentation for a keyword doc key (e.g. "if", "local", "class_const"),
/// loaded from the keyword_hovers.json resource. Returns nullopt if there's no documentation
/// registered for it.
std::optional<std::string> getKeywordHoverDocs(const std::string& keyword);

struct KeywordHoverMatch
{
    /// The keyword_hovers.json key that documents this position.
    std::string docKey;
    /// The full span the hover should highlight -- usually just the one keyword token, but for a
    /// combined doc key like "export_class" or "const_function" this covers both keyword tokens
    /// (e.g. all of `export class`), so the editor highlights the whole phrase, not just whichever
    /// half of it the cursor happened to land on.
    Luau::Location range;
};

/// If `position` lands on a keyword token, returns the keyword_hovers.json key that documents it,
/// along with the span that key's doc applies to.
///
/// The doc key is usually just the keyword's own literal spelling ("if", "local", "class", ...),
/// but some keywords carry different meaning depending on where they appear -- `const` on a class
/// field means something different from a local `const`, `export class` and `const function` are
/// worth documenting as a pair rather than as two separate keywords -- so the key isn't always a
/// literal substring of the source, and the returned range isn't always just the token at `position`.
///
/// `ancestry` is the root-to-innermost node chain at `position` (from `findAstAncestryOfPosition`).
/// Most keywords (e.g. `if`, `while`, `return`) are anchored directly on an AstStat/AstExpr node via
/// a dedicated keyword-location field, so checking `ancestry.back()` (the innermost node) is enough.
/// Class member qualifiers (`public`/`private`/`const`), a method's leading `function` keyword, and
/// a primary constructor's qualifiers (both the one on the constructor itself and the ones on its
/// parameters) are the exception: `AstClassProperty`/`AstClassMethod`/`AstClassPrimaryConstructor`
/// are plain structs, not AstNode subtypes, so the narrowest-enclosing-node search never lands on
/// them directly -- instead we walk up `ancestry` to the nearest `AstStatClass` and check its
/// members' and primary constructor's keyword locations explicitly.
std::optional<KeywordHoverMatch> findKeywordDocKeyAtPosition(const std::vector<Luau::AstNode*>& ancestry, Luau::Position position);

/// The keyword_hovers.json key documenting a comment directive (`--!strict`, `--!optimize 2`,
/// `--!trust`, ...), or nullopt when it is not one we recognise.
///
/// The key is the directive's name prefixed with `directive_`, taken from the first word of the
/// comment, so `--!optimize 2` and `--!nolint LocalShadow` are documented by the directive rather than
/// by their argument.
///
/// Colouring directives is the grammar's job (`meta.directive.luau` and `meta.directive.codegen.luau`
/// in Luau.tmLanguage.json), not the server's: a static grammar can tell the two kinds apart just as
/// well, and a semantic token would override it with whatever the theme does for an unmapped type.
std::optional<std::string> commentDirectiveDocKey(const Luau::HotComment& hotcomment);

/// If `position` lands on a comment directive, returns the key documenting it along with the whole
/// directive's span.
///
/// Directives are not AST nodes, so the keyword lookup above cannot see them: they arrive separately as
/// `SourceModule::hotcomments`. Hover has to consult this *before* it gives up on positions inside
/// comments, which is otherwise the right thing to do for an ordinary comment.
std::optional<KeywordHoverMatch> findCommentDirectiveDocKeyAtPosition(const std::vector<Luau::HotComment>& hotcomments, Luau::Position position);
