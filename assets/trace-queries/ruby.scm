;; limits: calls without parentheses or arguments, send, method_missing and define_method are not followed.
;; ANTS-5734 § 2.4 — trace queries for Ruby. Starts from upstream's tags.scm.

(method name: (_) @name) @definition.method
(singleton_method name: (_) @name) @definition.method
(alias name: (_) @name) @definition.method
(class name: (constant) @name) @definition.class
(class name: (scope_resolution name: (_) @name)) @definition.class
(module name: (constant) @name) @definition.module
(module name: (scope_resolution name: (_) @name)) @definition.module

(class name: (constant) @name) @scope
(module name: (constant) @name) @scope

(call method: (identifier) @name) @reference.call
