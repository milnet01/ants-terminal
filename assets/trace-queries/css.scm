;; limits: selectors built in script, nested selectors and custom properties are not followed.
;; ANTS-5734 § 2.4 — trace queries for CSS. A rule set is the definition of the
;; class or id its selector names.

(rule_set (selectors (class_selector (class_name (identifier) @name)))) @definition.class
(rule_set (selectors (id_selector (id_name) @name))) @definition.constant
(keyframes_statement (keyframes_name) @name) @definition.function

(import_statement (string_value) @path) @reference.import
