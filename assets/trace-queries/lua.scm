;; limits: calls through table fields assigned at run time are not followed.
;; ANTS-5734 § 2.4 — trace queries for Lua.

(function_declaration name: (identifier) @name) @definition.function
(function_declaration
  name: (dot_index_expression field: (identifier) @name)) @definition.function
(function_declaration
  name: (method_index_expression method: (identifier) @name)) @definition.method

(function_call name: (identifier) @name) @reference.call
(function_call
  name: (dot_index_expression field: (identifier) @name)) @reference.call
(function_call
  name: (method_index_expression method: (identifier) @name)) @reference.call
