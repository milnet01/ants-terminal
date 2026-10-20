;; limits: functions passed as values, `with` scopes and imported files are not followed.
;; ANTS-5734 § 2.4 — trace queries for Nix. Upstream's tags.scm captures a whole attribute path as the name and only calls of two or more arguments; these capture the last attribute and every call.

(binding attrpath: (attrpath attr: (identifier) @name .) expression: (function_expression)) @definition.function

(apply_expression function: (variable_expression name: (identifier) @name)) @reference.call
(apply_expression function: (select_expression attrpath: (attrpath attr: (identifier) @name .))) @reference.call
