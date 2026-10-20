;; limits: @import, comptime-generated declarations and calls through function pointers are not followed.
;; ANTS-5734 § 2.4 — trace queries for Zig. Upstream ships no tags.scm; written against tree-sitter-zig v1.1.2's node types.

(function_declaration name: (identifier) @name) @definition.function
(variable_declaration (identifier) @name (struct_declaration)) @definition.class
(variable_declaration (identifier) @name (enum_declaration)) @definition.type
(variable_declaration (identifier) @name (union_declaration)) @definition.type
(variable_declaration (identifier) @name (opaque_declaration)) @definition.type

(variable_declaration (identifier) @name (struct_declaration)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression function: (field_expression member: (identifier) @name)) @reference.call
