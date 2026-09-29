cmake_minimum_required(VERSION 3.19)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")

option(LUAU_ENABLE_TIME_TRACE "Build with Luau TimeTrace" OFF)
option(LSP_BUILD_ASAN "Build with ASAN" OFF)
option(LSP_STATIC_CRT "Link with the static CRT (/MT)" OFF)
option(LSP_BUILD_WITH_SENTRY "Build with Sentry (crash reporting)" OFF)
option(LSP_WERROR "Warnings as errors" ON)

set(LSP_VERSION "1.69.0")
set(LSP_NAME "Luwu")

if (LSP_STATIC_CRT)
    set(LUAU_STATIC_CRT ON)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

if (LUAU_ENABLE_TIME_TRACE)
    add_compile_definitions(LUAU_ENABLE_TIME_TRACE)
endif ()

project(Luwu.LanguageServer LANGUAGES CXX)

# Sentry setup
if (LSP_BUILD_WITH_SENTRY)
    include(FetchContent)
    set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BACKEND "crashpad" CACHE STRING "" FORCE)
    set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SENTRY_BUILD_RUNTIMESTATIC ${LSP_STATIC_CRT} CACHE BOOL "" FORCE)
    set(SENTRY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(CRASHPAD_ENABLE_STACKTRACE ON CACHE BOOL "" FORCE)
    FetchContent_Declare(sentry
        URL https://github.com/getsentry/sentry-native/releases/download/0.11.0/sentry-native.zip
        URL_HASH SHA256=2946d5d13ef2aa791fc4cee3663658da78ad8b3260f6976e1fd8f6e237c14db0
    )
    FetchContent_MakeAvailable(sentry)
endif ()

set(LSP_LUAU_PATH "${CMAKE_CURRENT_SOURCE_DIR}/luwu" CACHE PATH "Path to the Luau source tree to build against (defaults to the luwu submodule)")
add_subdirectory(${LSP_LUAU_PATH} luwu)
# luwu renamed its CMake targets from Luau.* to Luwu.*; a luwu checkout from before that (an older
# ./luwu submodule) still has the old names
if (TARGET Luwu.Analysis)
    set(LUWU_TARGET_PREFIX Luwu)
else ()
    set(LUWU_TARGET_PREFIX Luau)
endif ()
add_library(Luwu.LanguageServer STATIC)

# Regenerated whenever one of the JSON files changes.
# `|`-separated rather than a CMake list, which add_custom_command would split into separate arguments
set(LSP_EMBEDDED_RESOURCES
    "hovers=${CMAKE_CURRENT_SOURCE_DIR}/hovers.json|luwuDocs=${CMAKE_CURRENT_SOURCE_DIR}/luwu_docs.json")
set(LSP_EMBEDDED_RESOURCES_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedResources.cpp")
add_custom_command(
    OUTPUT "${LSP_EMBEDDED_RESOURCES_SOURCE}"
    COMMAND "${CMAKE_COMMAND}"
        "-DOUTPUT=${LSP_EMBEDDED_RESOURCES_SOURCE}"
        "-DRESOURCES=${LSP_EMBEDDED_RESOURCES}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/hovers.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/luwu_docs.json"
    COMMENT "Embedding hovers.json and luwu_docs.json"
    VERBATIM
)
add_executable(Luwu.LanguageServer.CLI)
add_executable(Luwu.LanguageServer.Test)

target_sources(Luwu.LanguageServer PRIVATE
        src/LanguageServer.cpp
        src/JsonRpc.cpp
        src/Uri.cpp
        src/WorkspaceFileResolver.cpp
        src/Workspace.cpp
        src/TextDocument.cpp
        src/Client.cpp
        src/DocumentationParser.cpp
        src/Hovers.cpp
        ${LSP_EMBEDDED_RESOURCES_SOURCE}
        src/LuauExt.cpp
        src/LuauFileUtils.cpp
        src/IostreamHelpers.cpp
        src/Utils.cpp
        src/Flags.cpp
        src/Thread.cpp
        src/JsonTomlSyntaxParser.cpp
        src/CliConfigurationParser.cpp
        src/platform/AutoImports.cpp
        src/platform/LSPPlatform.cpp
        src/platform/StringRequireAutoImporter.cpp
        src/platform/StringRequireSuggester.cpp
        src/platform/roblox/InstanceRequireAutoImporter.cpp
        src/platform/roblox/RobloxCodeAction.cpp
        src/platform/roblox/RobloxColorProvider.cpp
        src/platform/roblox/RobloxCompletion.cpp
        src/platform/roblox/RobloxFileResolver.cpp
        src/platform/roblox/RobloxLanguageServer.cpp
        src/platform/roblox/RobloxLuauExt.cpp
        src/platform/roblox/RobloxSourcemap.cpp
        src/platform/roblox/RobloxStringRequireSuggester.cpp
        src/platform/roblox/RobloxSourceNode.cpp
        src/platform/roblox/RobloxStudioPlugin.cpp
        src/operations/Diagnostics.cpp
        src/operations/Completion.cpp
        src/operations/DocumentSymbol.cpp
        src/operations/DocumentLink.cpp
        src/operations/ColorProvider.cpp
        src/operations/GotoDefinition.cpp
        src/operations/Hover.cpp
        src/operations/SignatureHelp.cpp
        src/operations/References.cpp
        src/operations/Rename.cpp
        src/operations/InlayHints.cpp
        src/operations/SemanticTokens.cpp
        src/operations/CodeAction.cpp
        src/operations/Refactoring.cpp
        src/operations/WorkspaceSymbol.cpp
        src/operations/FoldingRange.cpp
        src/operations/CallHierarchy.cpp
        src/operations/Bytecode.cpp
        src/operations/RequireGraph.cpp
        src/operations/OnTypeFormatting.cpp
        src/operations/MatchingKeyword.cpp
        src/operations/FileOperations.cpp
        src/transport/PipeTransport.cpp
        src/transport/StdioTransport.cpp
        src/Plugin/SourceMapping.cpp
        src/Plugin/PluginTextDocument.cpp
        src/Plugin/PluginRuntime.cpp
        src/Plugin/PluginManager.cpp
        src/Plugin/PluginLuaApi.cpp
        src/Plugin/PluginDefinitions.cpp
        extern/glob/match.cpp
)

target_sources(Luwu.LanguageServer.CLI PRIVATE
        src/main.cpp
        src/AnalyzeCli.cpp
)

target_sources(Luwu.LanguageServer.Test PRIVATE
        tests/main.cpp
        tests/Fixture.cpp
        tests/TempDir.cpp
        tests/TestClient.cpp
        tests/Autocomplete.test.cpp
        tests/AutoImports.test.cpp
        tests/Bytecode.test.cpp
        tests/MagicFunctions.test.cpp
        tests/Documentation.test.cpp
        tests/TextDocument.test.cpp
        tests/Uri.test.cpp
        tests/Utils.test.cpp
        tests/WorkspaceFileResolver.test.cpp
        tests/SemanticTokens.test.cpp
        tests/Sourcemap.test.cpp
        tests/References.test.cpp
        tests/Rename.test.cpp
        tests/ColorProvider.test.cpp
        tests/LuauExt.test.cpp
        tests/CliConfigurationParser.test.cpp
        tests/Hover.test.cpp
        tests/LanguageServer.test.cpp
        tests/InlayHints.test.cpp
        tests/JsonTomlSyntaxParser.test.cpp
        tests/Definitions.test.cpp
        tests/SignatureHelp.test.cpp
        tests/Diagnostics.test.cpp
        tests/GoToDefinition.test.cpp
        tests/Glob.test.cpp
        tests/Workspace.test.cpp
        tests/WorkspaceSymbol.test.cpp
        tests/RequireGraph.test.cpp
        tests/OnTypeFormatting.test.cpp
        tests/MatchingKeyword.test.cpp
        tests/FileOperations.test.cpp
        tests/AnalyzeCli.test.cpp
        tests/CodeAction.test.cpp
        tests/Plugin.test.cpp
        tests/Refactoring.test.cpp
        src/AnalyzeCli.cpp # For testing
)

# TODO: Set luwu's Analysis target at O2 to speed up debugging
if (MSVC)
    list(APPEND LUAU_LSP_OPTIONS /W3 /D_CRT_SECURE_NO_WARNINGS /bigobj)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        list(APPEND LUAU_LSP_OPTIONS "/EHsc") # The CMake clang-cl target doesn't enable exceptions by default
    else ()
        list(APPEND LUAU_LSP_OPTIONS /MP) # Distribute compilation across multiple cores; doesn't work with clang-cl
    endif ()
else ()
    list(APPEND LUAU_LSP_OPTIONS -Wall)
endif ()

if (LSP_WERROR)
    if (MSVC)
        list(APPEND LUAU_LSP_OPTIONS /WX) # Warnings are errors
    else ()
        list(APPEND LUAU_LSP_OPTIONS -Werror) # Warnings are errors
    endif ()
endif ()

if (LSP_BUILD_ASAN)
    if (MSVC)
        set(ASAN_FLAG /fsanitize=address)
    else ()
        set(ASAN_FLAG -fsanitize=address)
    endif ()

    list(APPEND LUAU_LSP_OPTIONS ${ASAN_FLAG})

    set(LUAU_ASAN_TARGETS ${LUWU_TARGET_PREFIX}.Ast ${LUWU_TARGET_PREFIX}.Analysis ${LUWU_TARGET_PREFIX}.Compiler ${LUWU_TARGET_PREFIX}.Config ${LUWU_TARGET_PREFIX}.VM ${LUWU_TARGET_PREFIX}.CodeGen ${LUWU_TARGET_PREFIX}.Common)
    foreach (TARGET_NAME IN LISTS LUAU_ASAN_TARGETS)
        target_compile_options(${TARGET_NAME} PRIVATE ${ASAN_FLAG})
    endforeach ()

    if (NOT MSVC)
        foreach (TARGET_NAME IN LISTS LUAU_ASAN_TARGETS ITEMS Luwu.LanguageServer Luwu.LanguageServer.CLI Luwu.LanguageServer.Test)
            target_link_options(${TARGET_NAME} PRIVATE ${ASAN_FLAG})
        endforeach ()
    endif ()
endif ()

set(EXTERN_INCLUDES extern/json/include extern/glob/include extern/argparse/include extern/toml/include extern/ryml/include)

target_compile_features(Luwu.LanguageServer PUBLIC cxx_std_17)
target_compile_options(Luwu.LanguageServer PRIVATE ${LUAU_LSP_OPTIONS})
target_compile_definitions(Luwu.LanguageServer PUBLIC
    LSP_VERSION="${LSP_VERSION}"
    LSP_NAME="${LSP_NAME}"
    $<$<BOOL:${LSP_BUILD_WITH_SENTRY}>:LSP_BUILD_WITH_SENTRY>
)
target_include_directories(Luwu.LanguageServer PUBLIC src/include)
target_include_directories(Luwu.LanguageServer SYSTEM PUBLIC ${EXTERN_INCLUDES})
target_link_libraries(Luwu.LanguageServer PRIVATE ${LUWU_TARGET_PREFIX}.Ast ${LUWU_TARGET_PREFIX}.Analysis ${LUWU_TARGET_PREFIX}.Compiler ${LUWU_TARGET_PREFIX}.CodeGen)
target_link_libraries(Luwu.LanguageServer PUBLIC ${LUWU_TARGET_PREFIX}.VM)

set_target_properties(Luwu.LanguageServer.CLI PROPERTIES OUTPUT_NAME luwu-lsp)
target_compile_options(Luwu.LanguageServer.CLI PRIVATE ${LUAU_LSP_OPTIONS})
target_include_directories(Luwu.LanguageServer.CLI PRIVATE src/include)
target_include_directories(Luwu.LanguageServer.CLI SYSTEM PRIVATE ${EXTERN_INCLUDES})
target_link_libraries(Luwu.LanguageServer.CLI PRIVATE Luwu.LanguageServer ${LUWU_TARGET_PREFIX}.Analysis)

target_compile_options(Luwu.LanguageServer.Test PRIVATE ${LUAU_LSP_OPTIONS})
target_include_directories(Luwu.LanguageServer.Test PRIVATE tests)
target_include_directories(Luwu.LanguageServer.Test SYSTEM PRIVATE ${EXTERN_INCLUDES} extern/doctest)
target_link_libraries(Luwu.LanguageServer.Test PRIVATE ${LUWU_TARGET_PREFIX}.Ast ${LUWU_TARGET_PREFIX}.Analysis Luwu.LanguageServer)

if (LSP_BUILD_WITH_SENTRY)
    target_link_libraries(Luwu.LanguageServer PRIVATE sentry)
    target_link_libraries(Luwu.LanguageServer.CLI PRIVATE sentry)

    add_custom_command(
            TARGET Luwu.LanguageServer.CLI POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:crashpad_handler>"
            "$<TARGET_FILE_DIR:Luwu.LanguageServer.CLI>"
            COMMENT "Copying crashpad_handler to output directory"
    )
endif ()
