;; limits: calls through function references, operator overloads and extension receivers are not followed.
;; ANTS-5734 § 2.4 — trace queries for Kotlin. Upstream ships no tags.scm; written against tree-sitter-kotlin 0.3.8's node types.

(package_header (identifier) @name) @definition.module
(class_declaration (type_identifier) @name) @definition.class
(object_declaration (type_identifier) @name) @definition.class
(function_declaration (simple_identifier) @name) @definition.function
(type_alias (type_identifier) @name) @definition.type

(class_declaration (type_identifier) @name (class_body)) @scope
(object_declaration (type_identifier) @name (class_body)) @scope

(call_expression (simple_identifier) @name) @reference.call
(call_expression (navigation_expression (navigation_suffix (simple_identifier) @name))) @reference.call
