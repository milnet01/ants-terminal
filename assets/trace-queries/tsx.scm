;; limits: calls through computed properties, call/apply and dynamic imports are not followed.
;; ANTS-5734 § 2.4 — trace queries for TSX (the same as typescript.scm).

(function_declaration name: (identifier) @name) @definition.function
(generator_function_declaration name: (identifier) @name) @definition.function
(function_signature name: (identifier) @name) @declaration.function
(method_definition name: (property_identifier) @name) @definition.method
(method_signature name: (property_identifier) @name) @declaration.method
(abstract_method_signature name: (property_identifier) @name) @declaration.method
(class_declaration name: (type_identifier) @name) @definition.class
(abstract_class_declaration name: (type_identifier) @name) @definition.class
(interface_declaration name: (type_identifier) @name) @definition.type
(type_alias_declaration name: (type_identifier) @name) @definition.type
(enum_declaration name: (identifier) @name) @definition.type
(internal_module name: (_) @name) @definition.module
(lexical_declaration
  (variable_declarator
    name: (identifier) @name
    value: [(arrow_function) (function_expression)])) @definition.function

(class_declaration name: (type_identifier) @name body: (_)) @scope
(abstract_class_declaration name: (type_identifier) @name body: (_)) @scope
(interface_declaration name: (type_identifier) @name body: (_)) @scope
(internal_module name: (_) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (member_expression property: (property_identifier) @name)) @reference.call
(new_expression constructor: (identifier) @name) @reference.call

(import_statement source: (string) @path) @reference.import
