;; limits: commands invoked through cmake_language(CALL) or a variable are not followed.
;; ANTS-5734 § 2.4 — trace queries for CMake. Upstream ships no tags.scm; written against tree-sitter-cmake v0.7.5's node types.

(function_def (function_command (argument_list . (argument) @name))) @definition.function
(macro_def (macro_command (argument_list . (argument) @name))) @definition.macro

(normal_command (identifier) @name) @reference.call
