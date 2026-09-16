#pragma once
#include <optional>
#include "Luau/AstQuery.h"
#include "Luau/Frontend.h"
#include "Luau/Scope.h"
#include "Luau/ToString.h"
#include "LSP/WorkspaceFileResolver.hpp"
#include "Protocol/Structures.hpp"
#include "Protocol/Diagnostics.hpp"

#include "nlohmann/json.hpp"

namespace types
{
std::optional<std::string> getTypeName(Luau::TypeId typeId);

bool isMetamethod(const Luau::Name& name);

std::optional<nlohmann::json> parseDefinitionsFileMetadata(const std::string& definitions);

Luau::LoadDefinitionFileResult registerDefinitions(
    Luau::Frontend& frontend, Luau::GlobalTypes& globals, const std::string& packageName, const std::string& definitions);

using NameOrExpr = std::variant<std::string, Luau::AstExpr*>;

// Converts a FTV and function call to a nice string
// In the format "function NAME(args): ret"
struct ToStringNamedFunctionOpts
{
    bool hideTableKind = false;
    bool multiline = false;
    bool hideFirstParameter = false;
    // If true, the first parameter (`self`) is printed bare (`self`) with no type annotation,
    // rather than being hidden entirely. Ignored unless hideFirstParameter is false.
    bool hideFirstParameterType = false;
    // Indentation of the line the signature is printed on (matching the "function"/"public"
    // keyword). When the signature has more than 3 parameters, they're broken out one per line,
    // indented one level deeper than this, with the closing "): ReturnType" brought back to this
    // indentation -- Rust-style. Ignored when `multiline` is set.
    std::string baseIndent = "";
};

// Wraps any over-long function-type line of a printed type (a table property whose type is a
// function, say) Rust-style with one parameter per line -- the same treatment a long named function
// signature gets -- so a hover doesn't run off the side of the screen.
std::string formatLongFunctionTypeLines(const std::string& typeString);

std::string toStringNamedFunction(const Luau::ModulePtr& module, const Luau::FunctionType* ftv, const NameOrExpr nameOrFuncExpr,
    std::optional<Luau::ScopePtr> scope = std::nullopt, const ToStringNamedFunctionOpts& opts = {});

std::string toStringReturnType(Luau::TypePackId retTypes, Luau::ToStringOptions options = {});
Luau::ToStringResult toStringReturnTypeDetailed(Luau::TypePackId retTypes, Luau::ToStringOptions options = {});

// Duplicated from Luau/TypeInfer.h, since its static
std::optional<Luau::AstExpr*> matchRequire(const Luau::AstExprCall& call);

std::optional<lsp::Location> getTypeLocation(Luau::TypeId ty, WorkspaceFileResolver* fileResolver);

// Finds the innermost class statement in `root` containing `position`, including positions inside
// its methods -- unlike `findClassStatContainingPosition`, which only counts the class body itself.
Luau::AstStatClass* findEnclosingClassStat(Luau::AstStatBlock* root, const Luau::Position& position);

// Finds the innermost class statement in `root` whose body contains `position`, if any.
Luau::AstStatClass* findClassStatContainingPosition(Luau::AstStatBlock* root, const Luau::Position& position);

// Finds every reference to a class's own name: its declaration, all value usages (constructor
// calls, static/method access via `ClassName.member`, passing the class around), and all type
// annotation usages (`local x: ClassName`).
std::vector<Luau::Location> findClassNameReferences(const Luau::SourceModule& source, Luau::AstStatClass* classStat);

// Finds every reference to a field, method, or static function declared on `classStat`: its
// declaration, and every `.name`/`:name` access site whose base expression's type resolves
// (directly, or via the class/object nominal relation) to `classStat`.
std::vector<Luau::Location> findClassMemberReferences(
    const Luau::SourceModule& source, const Luau::ModulePtr& module, Luau::AstStatClass* classStat, const Luau::AstName& memberName);

// If `ty` is (or is nominally related to, via the class/object relation) the extern type produced
// by some class statement in `root`, returns that class statement.
Luau::AstStatClass* findClassStatFromExternType(Luau::AstStatBlock* root, Luau::TypeId ty);

} // namespace types

// TODO: should upstream this
Luau::AstNode* findNodeOrTypeAtPosition(const Luau::SourceModule& source, Luau::Position pos);
Luau::AstNode* findNodeOrTypeAtPositionClosed(const Luau::SourceModule& source, Luau::Position pos);
Luau::ExprOrLocal findExprOrLocalAtPositionClosed(const Luau::SourceModule& source, Luau::Position pos);
std::vector<Luau::Location> findSymbolReferences(const Luau::SourceModule& source, Luau::Symbol symbol);
std::vector<Luau::Location> findTypeReferences(const Luau::SourceModule& source, const Luau::Name& typeName, std::optional<const Luau::Name> prefix);

std::optional<Luau::Location> getLocation(Luau::TypeId type);

std::optional<Luau::Location> lookupTypeLocation(const Luau::Scope& deepScope, const Luau::Name& name);

struct PropLookup
{
    Luau::TypeId baseTableTy;
    Luau::Property property;
};

std::vector<PropLookup> lookupProp(const Luau::TypeId& parentType, const Luau::Name& name);
std::optional<Luau::ModuleName> lookupImportedModule(const Luau::Scope& deepScope, const Luau::Name& name);

// Converts a UTF-8 position to a UTF-16 position, using the provided text document if available
// NOTE: if the text document doesn't exist, we perform no conversion, so the positioning may be
// incorrect
lsp::Position toUTF16(const TextDocument* textDocument, const Luau::Position& position);

lsp::Diagnostic createTypeErrorDiagnostic(const Luau::TypeError& error, Luau::FileResolver* fileResolver, const TextDocument* textDocument = nullptr);
lsp::Diagnostic createLintDiagnostic(const Luau::LintWarning& lint, const TextDocument* textDocument = nullptr);
lsp::Diagnostic createParseErrorDiagnostic(const Luau::ParseError& error, const TextDocument* textDocument = nullptr);

bool isGetService(const Luau::AstExpr* expr);
bool isRequire(const Luau::AstExpr* expr);
bool isMethod(const Luau::FunctionType* ftv);
bool isOverloadedMethod(Luau::TypeId ty);
std::optional<Luau::TypeId> findCallMetamethod(Luau::TypeId type);