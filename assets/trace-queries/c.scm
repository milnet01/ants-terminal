;; limits: calls through function pointers, and calls a macro expands to, are not followed.
;; ANTS-5734 § 2.4 — trace queries for C. Captures: @definition.<kind>,
;; @declaration.<kind>, @reference.call, @reference.import, @scope, each
;; with a nested @name (or @path for an import).

(function_definition
  declarator: (function_declarator declarator: (identifier) @name)) @definition.function
(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator declarator: (identifier) @name))) @definition.function

(declaration
  declarator: (function_declarator declarator: (identifier) @name)) @declaration.function
(declaration
  declarator: (pointer_declarator
    declarator: (function_declarator declarator: (identifier) @name))) @declaration.function

(struct_specifier name: (type_identifier) @name body: (_)) @definition.class
(union_specifier name: (type_identifier) @name body: (_)) @definition.class
(enum_specifier name: (type_identifier) @name body: (_)) @definition.type
(type_definition declarator: (type_identifier) @name) @definition.type
(preproc_def name: (identifier) @name) @definition.macro
(preproc_function_def name: (identifier) @name) @definition.macro
(enumerator name: (identifier) @name) @definition.constant

(struct_specifier name: (type_identifier) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (field_expression field: (field_identifier) @name)) @reference.call

(preproc_include path: (_) @path) @reference.import
