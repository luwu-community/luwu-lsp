# zsh completion for `seal ./rebuild.luau`, luwu-lsp's build script. Source it from ~/.zshrc after compinit (after
# oh-my-zsh.sh when you use oh-my-zsh). Any other seal script completes files as before.
#
# Keep it in sync with the args.parse call at the top of rebuild.luau.

_luwu_rebuild() {
    # drop `seal` so the script's path is words[1] and its first argument words[2]
    shift words
    (( CURRENT-- ))

    case $words[2] in
        submodule-update)
            # the command's own options come after it, so it stands where the script's name did
            shift words
            (( CURRENT-- ))
            _arguments \
                '--branch=[switch which luwu branch ./luwu tracks before updating]:branch: '
            ;;
        vsix)
            shift words
            (( CURRENT-- ))
            _arguments \
                '--install[install the packaged .vsix into VS Code]'
            ;;
        *)
            _arguments \
                '(- *)'{-h,--help}'[show the help message]' \
                '(- *)--commands[list the top-level commands]' \
                '--luwu=[luwu directory to compile against]:luwu directory:_luwu_rebuild_luwu_dirs' \
                '--targets=[CMake targets to build, separated by commas]:target:_sequence -s , compadd - CLI Test' \
                '--release[compile luwu-lsp in Release mode]' \
                '--static[fully statically link luwu-lsp into ./build-static (Linux)]' \
                '--clean[remove the build dir and reconfigure]' \
                '--full-debug-info[compile with -g instead of -g1]' \
                '1:command:((submodule-update\:"update ./luwu to the latest commit on its branch" vsix\:"package editors/code into a .vsix"))'
            ;;
    esac
}

# `--luwu=here` builds against ./luwu and `--luwu=test` against LUWU_TEST_PATH. Any directory works too.
_luwu_rebuild_luwu_dirs() {
    _alternative \
        'keywords:keyword:((here\:"the ./luwu submodule" test\:"LUWU_TEST_PATH"))' \
        'directories:directory:_directories'
}

_seal_with_rebuild() {
    if (( CURRENT > 2 )) && [[ $words[2] == (|*/)rebuild.luau ]]; then
        _luwu_rebuild
    else
        _files
    fi
}

compdef _seal_with_rebuild seal
