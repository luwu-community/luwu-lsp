; Doc comments hold Markdown. Zed's Markdown then highlights what it finds inside: `[Type.member]` references as
; links, code spans, and fenced code blocks with the language they name, so ```luwu examples get Luwu's colors.

; `--- text` lines: the content node is the text after `--`, one Markdown document per line
((comment
   content: (comment_content) @injection.content) @_comment
  (#match? @_comment "^---")
  (#set! injection.language "markdown"))

; `--[=[ ... ]=]` blocks. The grammar (our fork) gives one content node per line with the indentation left out,
; because Markdown would read indented lines as a code block. Two injections cover them: one Markdown document
; per line, which is what styles inline code spans and references (Zed's inline Markdown doesn't survive a
; combined layer), and one combined document for the whole block, which is what makes fenced code blocks work.
((comment
   content: (comment_content) @injection.content) @_comment
  (#match? @_comment "^--\\[=+\\[")
  (#set! injection.language "markdown"))

((comment
   content: (comment_content) @injection.content) @_comment
  (#match? @_comment "^--\\[=+\\[")
  (#set! injection.language "markdown")
  (#set! injection.combined))

; Everything else is a plain comment (TODO and FIXME get highlighted)
((comment) @injection.content
  (#not-match? @injection.content "^--(-|\\[=+\\[)")
  (#set! injection.language "comment"))
