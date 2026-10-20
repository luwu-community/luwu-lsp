#pragma once

#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Luau/Ast.h"
#include "Luau/Module.h"
#include "Luau/NotNull.h"
#include "Luau/Type.h"
#include "LSP/TextDocument.hpp"
#include "Protocol/Structures.hpp"

// Luwu Destructuring (rfcs/destructuring.md): reading and rewriting `.{...}` patterns, shared by the code actions that
// lay them out, import fields into them and qualify fields back out, and by completion's auto-imports through them.
// Also the comma-separated list helpers those build on.
namespace Luau::LanguageServer::Destructure
{

// Deleting a statement by deleting its lines is only safe when nothing else is on them
bool locationOwnsItsLines(const TextDocument& textDocument, const Luau::Location& location);

// The leading whitespace of a source line, used to match the document's existing indentation
std::string getLineIndentation(const TextDocument& textDocument, size_t line);

// One level of indentation, as the document already uses it, falling back to four spaces
std::string detectIndentUnit(const TextDocument& textDocument);

// A list's contents split on top-level commas, so commas inside an element's own brackets don't split it
std::vector<std::string> splitParameters(const std::string& parameters);

std::string layOutOnOneLine(const std::vector<std::string>& elements);

// A pattern's `.` (or whatever comes before its `{`) and its fields as written, with a trailing comma dropped. None when
// it holds a comment, which a new layout could put in front of code.
std::optional<std::pair<std::string, std::vector<std::string>>> splitDestructurePattern(const std::string& text);

// `.{ a, b.{ c, d } }`: the pattern and every pattern nested in it on one line. `pattern` is null for a type destructure.
std::optional<std::string> layOutDestructureOnOneLine(
    const Luau::Location& location, const Luau::AstDestructurePattern* pattern, const TextDocument& textDocument);

std::string layOutFieldsOnePerLine(const std::string& opening, const std::vector<std::string>& fields, size_t line, const TextDocument& textDocument);

std::optional<std::string> layOutDestructureOnePerLine(const Luau::Location& location, const TextDocument& textDocument);

// The pattern at `location` with `fields` in place of the ones it has, in the layout it already has
std::string layOutFieldsLike(
    const Luau::Location& location, const std::string& opening, const std::vector<std::string>& fields, const TextDocument& textDocument);

// Luwu Destructuring: what importing a field into a pattern (`components.Card` -> `Card`) and the reverse need to
// know about the file. The statements a destructuring desugars to are skipped: their `local.key` reads sit on the
// pattern's keys, and aren't uses anyone wrote.
struct DestructureFacts : Luau::AstVisitor
{
    // Every local declared on its own by a `local`/`const`, the desugared ones included
    std::unordered_map<Luau::AstLocal*, Luau::AstStatLocal*> declarations;
    // Every pattern, nested ones included, with the statement or loop holding the outermost one
    std::vector<std::pair<const Luau::AstDestructurePattern*, Luau::AstNode*>> patterns;
    // Only the outermost patterns, each with its statement or loop
    std::vector<std::pair<const Luau::AstDestructurePattern*, Luau::AstNode*>> topPatterns;
    std::vector<Luau::AstExprIndexName*> indexes;
    std::vector<Luau::AstExprLocal*> localRefs;
    std::vector<Luau::AstTypeReference*> typeRefs;
    std::unordered_set<const Luau::AstExpr*> assigned;
    std::unordered_set<std::string> globals;
    // The paths of every `require("...")`
    std::unordered_set<std::string> requirePaths;

    void addPattern(const Luau::AstDestructurePattern& pattern, Luau::AstNode* holder)
    {
        if (pattern.location)
            patterns.emplace_back(&pattern, holder);
        for (const auto& field : pattern.fields)
            addPattern(field.target, holder);
    }

    bool visit(Luau::AstStatLocal* node) override
    {
        if (node->vars.size == 1)
            declarations[node->vars.data[0]] = node;
        if (node->destructure)
        {
            topPatterns.emplace_back(node->destructure, node);
            addPattern(*node->destructure, node);
        }
        return !node->isDesugaredField();
    }

    bool visit(Luau::AstStatForIn* node) override
    {
        for (const auto* pattern : node->destructure)
            if (pattern)
            {
                topPatterns.emplace_back(pattern, node);
                addPattern(*pattern, node);
            }
        return true;
    }

    bool visit(Luau::AstStatTypeAlias* node) override
    {
        // `type .{A} = m` desugars to `type A = m.A`, whose reference sits on the key
        return !node->destructure && !node->destructuredFrom;
    }

    bool visit(Luau::AstStatAssign* node) override
    {
        for (auto* var : node->vars)
            assigned.insert(var);
        return true;
    }

    bool visit(Luau::AstStatCompoundAssign* node) override
    {
        assigned.insert(node->var);
        return true;
    }

    bool visit(Luau::AstExprIndexName* node) override
    {
        indexes.push_back(node);
        return true;
    }

    bool visit(Luau::AstExprLocal* node) override
    {
        localRefs.push_back(node);
        return true;
    }

    bool visit(Luau::AstExprGlobal* node) override
    {
        globals.insert(node->name.value);
        return true;
    }

    bool visit(Luau::AstExprCall* node) override
    {
        const auto* func = node->func->as<Luau::AstExprGlobal>();
        const auto* path = node->args.size == 1 ? node->args.data[0]->as<Luau::AstExprConstantString>() : nullptr;
        if (func && path && strcmp(func->name.value, "require") == 0)
            requirePaths.emplace(path->value.data, path->value.size);
        return true;
    }

    bool visit(Luau::AstTypeReference* node) override
    {
        typeRefs.push_back(node);
        return true;
    }

    bool visit(Luau::AstType* node) override
    {
        return true;
    }
};

bool isHiddenDestructureLocal(const Luau::AstLocal* local);
bool isIdentifier(const std::string& name);

// A class or trait value, which brings its type in along with it
bool bringsAType(std::optional<Luau::TypeId> ty, Luau::NotNull<Luau::BuiltinTypes> builtinTypes);

// Whether binding `name` would capture or shadow anything: any other local or global of that name anywhere in the file,
// or, when the binding brings a type in, a type of that name
bool nameIsTaken(const std::string& name, bool withType, const Luau::AstLocal* except, const Luau::Module& module, const DestructureFacts& facts);

std::optional<Luau::TypeId> typeOfLocal(Luau::AstLocal* local, const Luau::Module& module, const DestructureFacts& facts);
std::optional<Luau::TypeId> fieldType(std::optional<Luau::TypeId> ty, const std::string& name);

// The local a pattern's fields can be read through as `local.field`: the pattern's own name, or for an unnamed
// `const .{...} = chimera`, the local it destructures
Luau::AstLocal* patternQualifier(const Luau::AstDestructurePattern& pattern, const DestructureFacts& facts);

// Edits turning every `qualifier.name` in the file (and, for a class, every `qualifier.name` type) into `name`. None
// when one is assigned to, which a bare name would turn into assigning the import.
std::optional<std::vector<lsp::TextEdit>> unqualifyEdits(
    Luau::AstLocal* qualifier, const std::string& name, bool withType, const DestructureFacts& facts, const TextDocument& textDocument);

// Edits taking the fields that bind the locals in `drop` out of their patterns. A nested pattern left empty leaves its
// key bound on its own, or goes too when nothing reads the key; a declaration left binding nothing is deleted.
// `removed` gets the locals the edits take out: one in a pattern that can't be rewritten (it holds a comment) stays.
// `alsoRead` are keys to count as read, for edits that add reads of them.
std::vector<lsp::TextEdit> removeFieldsEdits(const std::unordered_set<const Luau::AstLocal*>& drop, const DestructureFacts& facts,
    const TextDocument& textDocument, std::unordered_set<const Luau::AstLocal*>* removed = nullptr,
    const std::unordered_set<const Luau::AstLocal*>& alsoRead = {});

// Whether `local` is bound by a field of a pattern, rather than declared on its own
bool isPatternField(const Luau::AstLocal* local, const DestructureFacts& facts);

// A name completion can offer from a module already imported through a destructuring: `Paragraph` from
// `const .{ elements.{ Heading } } = chimera`, imported by `edit` as `elements.{ Heading, Paragraph }`
struct ImportCandidate
{
    std::string name;
    // Where it comes from, `chimera.elements`
    std::string from;
    std::optional<Luau::TypeId> type;
    lsp::TextEdit edit;
};

// A module the file doesn't require yet, and where a require of it would go
struct UnimportedModule
{
    std::string requirePath;
    std::optional<Luau::TypeId> type;
    lsp::Range range;
    bool prependNewline = false;
    bool useConst = true;
};

// Every field up to two levels into a value the module's top level already destructures or binds from a `require`, or
// into a module in `unimported`, that isn't bound yet and wouldn't capture or shadow anything at `position`. Only names
// starting like `typed` are offered; `typesOnly` keeps the classes and traits, for a type annotation. A field a pattern
// already written can take comes before one needing a new declaration.
std::vector<ImportCandidate> importCandidates(const Luau::SourceModule& sourceModule, const Luau::Module& module, const Luau::Position& position,
    const std::string& typed, bool typesOnly, Luau::NotNull<Luau::BuiltinTypes> builtinTypes, const TextDocument& textDocument,
    const std::vector<UnimportedModule>& unimported = {});

} // namespace Luau::LanguageServer::Destructure
