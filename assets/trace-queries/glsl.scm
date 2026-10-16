;; limits: calls a macro expands to are not followed.
;; ANTS-5734 § 2.4 — trace queries for GLSL (a C-family grammar).

(function_definition
  declarator: (function_declarator declarator: (identifier) @name)) @definition.function
(declaration
  declarator: (function_declarator declarator: (identifier) @name)) @declaration.function
(struct_specifier name: (type_identifier) @name body: (_)) @definition.class
(preproc_def name: (identifier) @name) @definition.macro
(preproc_function_def name: (identifier) @name) @definition.macro

(struct_specifier name: (type_identifier) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (field_expression field: (field_identifier) @name)) @reference.call

(preproc_include path: (_) @path) @reference.import
