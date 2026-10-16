;; limits: calls through variables, getattr and decorators are not followed.
;; ANTS-5734 § 2.4 — trace queries for Python.

(function_definition name: (identifier) @name) @definition.function
(class_definition name: (identifier) @name) @definition.class

(class_definition name: (identifier) @name body: (_)) @scope

(call function: (identifier) @name) @reference.call
(call function: (attribute attribute: (identifier) @name)) @reference.call

(import_statement name: (dotted_name) @path) @reference.import
(import_statement name: (aliased_import name: (dotted_name) @path)) @reference.import
(import_from_statement module_name: (_) @path) @reference.import
