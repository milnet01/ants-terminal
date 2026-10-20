;; limits: messages to a selector held in a variable, performSelector: and categories are not followed; a keyword selector is named by its first part.
;; ANTS-5734 § 2.4 — trace queries for Objective-C. Upstream ships no tags.scm; written against tree-sitter-objc v3.0.2's node types.

(method_definition . (method_type) . (identifier) @name) @definition.method
(method_declaration . (method_type) . (identifier) @name) @declaration.method
(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function
(class_interface . (identifier) @name) @definition.class
(class_implementation . (identifier) @name) @definition.class
(protocol_declaration . (identifier) @name) @definition.type

(class_implementation . (identifier) @name) @scope
(class_interface . (identifier) @name) @scope

(message_expression receiver: (_) . method: (identifier) @name) @reference.call
(call_expression function: (identifier) @name) @reference.call

(preproc_include path: (_) @path) @reference.import
