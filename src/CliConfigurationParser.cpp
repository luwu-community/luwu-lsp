/// This is used only in the CLI to parse vscode-style configuration into ClientConfiguration

#include "Luau/StringUtils.h"
#include "Analyze/CliConfigurationParser.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

// Luwu: VS Code settings files are JSONC, which allows comments and trailing commas. Upstream parses them as strict
// JSON, so a settings file with either was dropped with only a parse error, and analyze ran without its fflag
// overrides. nlohmann skips comments itself (the last argument of `json::parse`), but this version has no option for
// trailing commas.
//
// Returns `contents` without the commas that come right before a `}` or `]`. Strings and comments are copied as they
// are, and the comma's lookahead skips whitespace and comments.
static std::string withoutTrailingCommas(const std::string& contents)
{
    std::string result;
    result.reserve(contents.size());

    // Where the comment starting at `i` ends: one past its last character, or `i` when no comment starts there
    auto commentEnd = [&](size_t i) -> size_t
    {
        if (i + 1 >= contents.size() || contents[i] != '/')
            return i;
        if (contents[i + 1] == '/')
        {
            size_t newline = contents.find('\n', i);
            return newline == std::string::npos ? contents.size() : newline;
        }
        if (contents[i + 1] == '*')
        {
            size_t close = contents.find("*/", i + 2);
            return close == std::string::npos ? contents.size() : close + 2;
        }
        return i;
    };

    size_t i = 0;
    while (i < contents.size())
    {
        char c = contents[i];
        if (c == '"')
        {
            // copy the string, escapes included, up to and including its closing quote
            size_t end = i + 1;
            while (end < contents.size() && contents[end] != '"')
                end += contents[end] == '\\' ? 2 : 1;
            end = std::min(end + 1, contents.size());
            result.append(contents, i, end - i);
            i = end;
            continue;
        }

        if (size_t end = commentEnd(i); end != i)
        {
            result.append(contents, i, end - i);
            i = end;
            continue;
        }

        if (c == ',')
        {
            size_t next = i + 1;
            while (next < contents.size())
            {
                if (isspace(static_cast<unsigned char>(contents[next])))
                    next++;
                else if (size_t end = commentEnd(next); end != next)
                    next = end;
                else
                    break;
            }
            bool closesNext = next < contents.size() && (contents[next] == '}' || contents[next] == ']');
            if (closesNext)
            {
                i++;
                continue;
            }
        }

        result.push_back(c);
        i++;
    }
    return result;
}

json parseDottedConfiguration(const std::string& contents)
{
    json data;

    try
    {
        data = json::parse(withoutTrailingCommas(contents), /* callback= */ nullptr, /* allow_exceptions= */ true, /* ignore_comments= */ true);
    }
    catch (const json::exception& err)
    {
        std::cerr << "Failed to parse settings JSON: " << err.what() << std::endl;
    }

    json output = json::object();


    for (const auto& el : data.items())
    {
        auto parts = Luau::split(el.key(), '.');

        // Remove the first part
        // TODO: we should warn if the first part is not "luau" or "luau-lsp"
        parts.erase(parts.begin());

        auto* current = &output;

        for (size_t i = 0; i < parts.size(); i++)
        {
            auto key = parts[i];
            if (i == parts.size() - 1)
            {
                current->emplace(key, el.value());
            }
            else
            {
                if (!current->contains(key))
                    current->emplace(key, json::object());
                current = &current->at(key);
            }
        }
    }

    return output;
}

ClientConfiguration dottedToClientConfiguration(const std::string& contents)
{
    return parseDottedConfiguration(contents);
}
