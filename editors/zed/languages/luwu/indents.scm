(if_statement "end_keyword" @end) @indent
(do_statement "end_keyword" @end) @indent
(while_statement "end_keyword" @end) @indent
(for_statement "end_keyword" @end) @indent
(repeat_statement "until" @end) @indent
(function_declaration "end_keyword" @end) @indent
(local_function_declaration "end_keyword" @end) @indent
(const_function_declaration "end_keyword" @end) @indent
(type_function_declaration "end_keyword" @end) @indent
(function_definition "end_keyword" @end) @indent

(_ "[" "]" @end) @indent
(_ "{" "}" @end) @indent
(_ "(" ")" @end) @indent
