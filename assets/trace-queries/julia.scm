;; limits: multiple dispatch is not resolved to a method, and calls built by macros are not followed.
;; ANTS-5734 § 2.4 — trace queries for Julia. Upstream ships no tags.scm; written against tree-sitter-julia v0.25.0's node types.

(function_definition (signature (call_expression . (identifier) @name))) @definition.function
(assignment . (call_expression . (identifier) @name)) @definition.function
(macro_definition (signature (call_expression . (identifier) @name))) @definition.function
(struct_definition (type_head . (identifier) @name)) @definition.class
(abstract_definition (type_head . (identifier) @name)) @definition.type
(module_definition name: (identifier) @name) @definition.module

(module_definition name: (identifier) @name) @scope

(call_expression . (identifier) @name) @reference.call
(call_expression . (field_expression (identifier) @name .)) @reference.call

(import_statement (identifier) @path) @reference.import
(using_statement (selected_import . (identifier) @path)) @reference.import
(using_statement (identifier) @path) @reference.import
