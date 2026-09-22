#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include <algorithm>

struct SourceNode;
class TextDocument;

/// Every source file extension the language server understands, with the leading dot.
/// `.luwu` is Luwu's own extension; `.luau` and `.lua` are accepted for compatibility.
extern const std::vector<std::string_view> kSourceFileExtensions;

/// Whether `extension` (with its leading dot, as `Uri::extension()` returns it) names a
/// source file the server can read. This is about what the server *understands*, not what
/// it has been asked to analyse -- `isEnabledSourceFileExtension` is the latter.
bool isSourceFileExtension(std::string_view extension);

/// Whether `extension` (with its leading dot) is one of the extensions the client asked us
/// to analyse, as `ClientConfiguration::fileExtensions` (dotless names, e.g. `"luau"`) lists
/// them. A file the server understands but that the client didn't list belongs to another
/// language server, so we neither index it nor report diagnostics for it.
bool isEnabledSourceFileExtension(const std::vector<std::string>& enabledExtensions, std::string_view extension);

/// `path` without its source file extension, if it has one. Only ever strips one.
std::string removeSourceFileExtension(const std::string& path);

std::optional<std::string> getParentPath(const std::string& path);
std::optional<std::string> getAncestorPath(const std::string& path, const std::string& ancestorName, const SourceNode* rootSourceNode);
std::string convertToScriptPath(std::string path);
std::string codeBlock(const std::string& language, const std::string& code);
/// Whether `textDocument` is a .luwu file (or the client says it holds Luwu code). Anything else -- .luau, .lua -- is
/// Luau code, where Luwu-only features such as the `--!trust` directive aren't allowed.
bool isLuwuFile(const TextDocument& textDocument);
/// The language of the markdown code blocks shown for `textDocument`: `luwu`, except `luau` for a file that isn't
/// a .luwu file in Luau compatibility mode. A .luwu file is always Luwu code; compatibility mode only exists for
/// .luau code written for Luau 0.730 or earlier.
const char* codeBlockLanguage(const TextDocument& textDocument);
/// `codeBlockLanguage` for a code block not tied to a file, such as a code sample from a documentation file
const char* codeBlockLanguage();
std::optional<std::string> getHomeDirectory();
std::string resolvePath(const std::string& path);
bool isDataModel(const std::string& path);
void trim_start(std::string& str);
void trim_end(std::string& str);
void trim(std::string& str);
std::string removePrefix(const std::string& str, const std::string& prefix);
std::string toLower(std::string str);
std::string_view getFirstLine(const std::string_view& str);
bool endsWith(const std::string_view& str, const std::string_view& suffix);
std::string removeSuffix(const std::string& str, const std::string_view& suffix);
bool replace(std::string& str, const std::string& from, const std::string& to);
void replaceAll(std::string& str, const std::string& from, const std::string& to);

template<typename V>
inline bool contains(const std::vector<V>& vec, const V& value)
{
    return std::find(std::begin(vec), std::end(vec), value) != std::end(vec);
}

template<class K, class V>
inline bool contains(const std::unordered_map<K, V>& map, const K& value)
{
    return map.find(value) != map.end();
}

template<class K, class V>
inline bool contains(const std::map<K, V>& map, const K& value)
{
    return map.find(value) != map.end();
}
