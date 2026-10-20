;; limits: implicit conversions, apply() calls through a value, and imports are not followed.
;; ANTS-5734 § 2.4 — trace queries for Scala. Starts from upstream's tags.scm, without its variable and parameter definitions.

(package_clause name: (package_identifier) @name) @definition.module
(trait_definition name: (identifier) @name) @definition.type
(enum_definition name: (identifier) @name) @definition.type
(class_definition name: (identifier) @name) @definition.class
(object_definition name: (identifier) @name) @definition.class
(function_definition name: (identifier) @name) @definition.function
(function_declaration name: (identifier) @name) @declaration.function
(type_definition name: (type_identifier) @name) @definition.type

(trait_definition name: (identifier) @name body: (_)) @scope
(class_definition name: (identifier) @name body: (_)) @scope
(object_definition name: (identifier) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression function: (field_expression field: (identifier) @name)) @reference.call
(instance_expression (type_identifier) @name) @reference.call
