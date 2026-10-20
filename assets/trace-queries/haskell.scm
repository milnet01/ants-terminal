;; limits: operators, point-free references and calls through a qualified name are not followed.
;; ANTS-5734 § 2.4 — trace queries for Haskell. Upstream ships no tags.scm; written against tree-sitter-haskell v0.24.1's node types.

(header module: (module) @name) @definition.module
(data_type name: (name) @name) @definition.type
(newtype name: (name) @name) @definition.type
(type_synonym name: (name) @name) @definition.type
(class name: (name) @name) @definition.class
(function name: (variable) @name) @definition.function
(bind name: (variable) @name) @definition.function
(signature name: (variable) @name) @declaration.function

(class name: (name) @name) @scope

(apply function: (variable) @name) @reference.call
