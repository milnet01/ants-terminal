;; limits: calls through delegates, events, interfaces and reflection are not followed.
;; ANTS-5734 § 2.4 — trace queries for C#. Starts from upstream's tags.scm.

(method_declaration name: (identifier) @name body: (_)) @definition.method
(method_declaration name: (identifier) @name !body) @declaration.method
(constructor_declaration name: (identifier) @name) @definition.method
(local_function_statement name: (identifier) @name) @definition.function
(property_declaration name: (identifier) @name) @definition.field
(class_declaration name: (identifier) @name) @definition.class
(struct_declaration name: (identifier) @name) @definition.class
(record_declaration name: (identifier) @name) @definition.class
(interface_declaration name: (identifier) @name) @definition.type
(enum_declaration name: (identifier) @name) @definition.type
(namespace_declaration name: (identifier) @name) @definition.module

(class_declaration name: (identifier) @name body: (_)) @scope
(struct_declaration name: (identifier) @name body: (_)) @scope
(interface_declaration name: (identifier) @name body: (_)) @scope

(invocation_expression function: (identifier) @name) @reference.call
(invocation_expression
  function: (member_access_expression name: (identifier) @name)) @reference.call
(object_creation_expression type: (identifier) @name) @reference.call

(using_directive [(identifier) (qualified_name)] @path) @reference.import
