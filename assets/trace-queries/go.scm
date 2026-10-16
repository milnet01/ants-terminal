;; limits: calls through interfaces and function values are not followed.
;; ANTS-5734 § 2.4 — trace queries for Go.

(function_declaration name: (identifier) @name) @definition.function
(method_declaration name: (field_identifier) @name) @definition.method
(type_spec name: (type_identifier) @name) @definition.type
(const_spec name: (identifier) @name) @definition.constant

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (selector_expression field: (field_identifier) @name)) @reference.call

(import_spec path: (_) @path) @reference.import
