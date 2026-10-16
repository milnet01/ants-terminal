;; limits: calls through trait objects, closures and macro bodies are not followed.
;; ANTS-5734 § 2.4 — trace queries for Rust.

(function_item name: (identifier) @name) @definition.function
(function_signature_item name: (identifier) @name) @declaration.function
(struct_item name: (type_identifier) @name) @definition.class
(enum_item name: (type_identifier) @name) @definition.type
(union_item name: (type_identifier) @name) @definition.type
(trait_item name: (type_identifier) @name) @definition.type
(type_item name: (type_identifier) @name) @definition.type
(mod_item name: (identifier) @name) @definition.module
(macro_definition name: (identifier) @name) @definition.macro
(const_item name: (identifier) @name) @definition.constant
(static_item name: (identifier) @name) @definition.constant

(impl_item type: (type_identifier) @name body: (_)) @scope
(trait_item name: (type_identifier) @name body: (_)) @scope
(mod_item name: (identifier) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (field_expression field: (field_identifier) @name)) @reference.call
(call_expression
  function: (scoped_identifier name: (identifier) @name)) @reference.call
(macro_invocation macro: (identifier) @name) @reference.call

(use_declaration argument: (_) @path) @reference.import
