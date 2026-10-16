;; limits: signal handlers, property bindings and calls through computed properties are not followed.
;; ANTS-5734 § 2.4 — trace queries for QML and its JavaScript.

(function_declaration name: (identifier) @name) @definition.function
(ui_property name: (identifier) @name) @definition.field
(ui_signal name: (identifier) @name) @definition.function

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (member_expression property: (property_identifier) @name)) @reference.call

(ui_import source: (_) @path) @reference.import
