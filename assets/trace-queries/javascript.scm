;; limits: calls through computed properties, call/apply and dynamic imports are not followed.
;; ANTS-5734 § 2.4 — trace queries for JavaScript.

(function_declaration name: (identifier) @name) @definition.function
(generator_function_declaration name: (identifier) @name) @definition.function
(method_definition name: (property_identifier) @name) @definition.method
(class_declaration name: (identifier) @name) @definition.class
(lexical_declaration
  (variable_declarator
    name: (identifier) @name
    value: [(arrow_function) (function_expression)])) @definition.function
(variable_declaration
  (variable_declarator
    name: (identifier) @name
    value: [(arrow_function) (function_expression)])) @definition.function

(class_declaration name: (identifier) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (member_expression property: (property_identifier) @name)) @reference.call
(new_expression constructor: (identifier) @name) @reference.call

(import_statement source: (string) @path) @reference.import
