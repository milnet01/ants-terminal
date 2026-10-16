;; limits: targets named through variables, pattern rules and $(call) are not followed.
;; ANTS-5734 § 2.4 — trace queries for Make. A prerequisite is a call of its target.

(rule (targets (word) @name)) @definition.function
(variable_assignment name: (word) @name) @definition.constant

(rule normal: (prerequisites (word) @name)) @reference.call

(include_directive filenames: (list (word) @path)) @reference.import
