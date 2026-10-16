;; limits: calls through interfaces, reflection and lambdas are not followed.
;; ANTS-5734 § 2.4 — trace queries for Java.

(method_declaration name: (identifier) @name body: (block)) @definition.method
(method_declaration name: (identifier) @name !body) @declaration.method
(constructor_declaration name: (identifier) @name) @definition.method
(class_declaration name: (identifier) @name) @definition.class
(record_declaration name: (identifier) @name) @definition.class
(interface_declaration name: (identifier) @name) @definition.type
(enum_declaration name: (identifier) @name) @definition.type

(class_declaration name: (identifier) @name body: (_)) @scope
(record_declaration name: (identifier) @name body: (_)) @scope
(interface_declaration name: (identifier) @name body: (_)) @scope
(enum_declaration name: (identifier) @name body: (_)) @scope

(method_invocation name: (identifier) @name) @reference.call
(object_creation_expression type: (type_identifier) @name) @reference.call

(import_declaration (scoped_identifier) @path) @reference.import
