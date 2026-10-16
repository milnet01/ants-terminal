;; limits: commands built from variables or run through eval are not followed.
;; ANTS-5734 § 2.4 — trace queries for Bash. A command is a call.

(function_definition name: (word) @name) @definition.function

(command name: (command_name (word) @name)) @reference.call
