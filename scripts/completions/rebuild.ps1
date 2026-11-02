# PowerShell completion for `seal ./rebuild.luau`, luwu-lsp's build script. Dot-source it from your $PROFILE:
#
#     . path\to\luwu-lsp\scripts\completions\rebuild.ps1
#
# Any other seal script completes files as before. Keep it in sync with the args.parse call at the top of rebuild.luau.

Register-ArgumentCompleter -Native -CommandName seal, seal.exe -ScriptBlock {
    param($wordToComplete, $commandAst, $cursorPosition)

    $commands = [ordered]@{
        'submodule-update' = 'update ./luwu to the latest commit on its branch'
        'vsix'             = 'package editors/code into a .vsix'
    }

    # The options of each command. The build itself is the command named ''.
    $commandOptions = @{
        '' = [ordered]@{
            '--luwu='           = 'luwu directory to compile against: here (./luwu), test (LUWU_TEST_PATH) or a path'
            '--targets='        = 'CMake targets to build, separated by commas: CLI, Test'
            '--release'         = 'compile luwu-lsp in Release mode'
            '--static'          = 'fully statically link luwu-lsp into ./build-static (Linux)'
            '--clean'           = 'remove the build dir and reconfigure'
            '--full-debug-info' = 'compile with -g instead of -g1'
            '--help'            = 'show the help message'
            '--commands'        = 'list the top-level commands'
        }
        'submodule-update' = [ordered]@{
            '--branch=' = 'switch which luwu branch ./luwu tracks before updating'
        }
        'vsix' = [ordered]@{
            '--install' = 'install the packaged .vsix into VS Code'
        }
    }

    $elements = @($commandAst.CommandElements | ForEach-Object { $_.Extent.Text })
    # The word being completed is the last element unless it's empty
    $done = if ($wordToComplete) { $elements.Count - 1 } else { $elements.Count }

    # Returning nothing leaves PowerShell to complete files
    if ($done -lt 2 -or $elements[1] -notmatch '(^|[\\/])rebuild\.luau$') {
        return
    }

    # PowerShell reads a bare comma as an array separator and a space as the next argument, so those get quotes
    $result = {
        param($text, $tooltip, $type = 'ParameterName')
        $completion = if ($text -match '[ ,]') { "'$text'" } else { $text }
        [System.Management.Automation.CompletionResult]::new($completion, $text, $type, $tooltip)
    }

    # A value after `--luwu=` or `--targets=`
    if ($wordToComplete -like '--luwu=*') {
        & $result '--luwu=here' 'the ./luwu submodule' 'ParameterValue'
        & $result '--luwu=test' 'LUWU_TEST_PATH' 'ParameterValue'
        $typed = $wordToComplete.Substring('--luwu='.Length)
        foreach ($dir in Get-ChildItem -Directory -Path "$typed*" -ErrorAction SilentlyContinue) {
            $path = Resolve-Path -Relative $dir.FullName
            & $result "--luwu=$path" $dir.FullName 'ProviderContainer'
        }
        return
    }
    if ($wordToComplete -like '--targets=*') {
        foreach ($targets in 'CLI', 'Test', 'CLI,Test') {
            & $result "--targets=$targets" "build $targets" 'ParameterValue'
        }
        return
    }

    $command = if ($done -gt 2 -and $commands.Contains($elements[2])) { $elements[2] } else { '' }

    # A command can only come right after the script
    if ($command -eq '' -and $done -eq 2) {
        foreach ($name in $commands.Keys) {
            if ($name -like "$wordToComplete*") {
                & $result $name $commands[$name] 'Command'
            }
        }
    }

    $options = $commandOptions[$command]
    foreach ($option in $options.Keys) {
        if ($option -like "$wordToComplete*" -and $elements -notcontains $option) {
            & $result $option $options[$option]
        }
    }
}
