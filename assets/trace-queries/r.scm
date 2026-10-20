;; limits: functions assigned with assign(), called through do.call() or a variable, and S4/R5 methods are not followed.
;; ANTS-5734 § 2.4 — trace queries for R. Upstream's tags.scm, unchanged but for these comments.

(binary_operator lhs: (identifier) @name operator: "<-" rhs: (function_definition)) @definition.function
(binary_operator lhs: (identifier) @name operator: "=" rhs: (function_definition)) @definition.function
(binary_operator lhs: (string) @name operator: "<-" rhs: (function_definition)) @definition.function
(binary_operator lhs: (string) @name operator: "=" rhs: (function_definition)) @definition.function

(call function: (identifier) @name) @reference.call
(call function: (namespace_operator rhs: (identifier) @name)) @reference.call
