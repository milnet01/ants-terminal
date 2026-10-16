;; limits: calls through function pointers, virtual dispatch and Qt signal and slot connections are not followed.
;; ANTS-5734 § 2.4 — trace queries for C++. Not upstream's tags.scm as
;; shipped: that has no call pattern, and its function_declarator pattern
;; also matches prototypes. @qualifier carries an out-of-line definition's
;; class or namespace (`void Foo::bar() {}`), added to its scope.

; Definitions with a body. The declarator may sit under a pointer or
; reference declarator (a function returning T* or T&).
(function_definition
  declarator: (function_declarator
    declarator: [(identifier) (operator_name)] @name)) @definition.function
(function_definition
  declarator: (function_declarator
    declarator: [(field_identifier) (destructor_name)] @name)) @definition.method
(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      scope: (_) @qualifier
      name: [(identifier) (destructor_name) (operator_name)] @name))) @definition.method
(function_definition
  declarator: (_
    (function_declarator
      declarator: [(identifier) (operator_name)] @name))) @definition.function
(function_definition
  declarator: (_
    (function_declarator
      declarator: (field_identifier) @name))) @definition.method
(function_definition
  declarator: (_
    (function_declarator
      declarator: (qualified_identifier
        scope: (_) @qualifier
        name: [(identifier) (operator_name)] @name)))) @definition.method

; Prototypes: a declaration, or a member declaration in a class body.
(declaration
  declarator: (function_declarator
    declarator: [(identifier) (operator_name)] @name)) @declaration.function
(declaration
  declarator: (function_declarator
    declarator: (qualified_identifier
      scope: (_) @qualifier
      name: (identifier) @name))) @declaration.function
(declaration
  declarator: (_
    (function_declarator declarator: (identifier) @name))) @declaration.function
(field_declaration
  declarator: (function_declarator
    declarator: [(field_identifier) (operator_name)] @name)) @declaration.method
(field_declaration
  declarator: (_
    (function_declarator declarator: (field_identifier) @name))) @declaration.method

(class_specifier name: (type_identifier) @name body: (_)) @definition.class
(struct_specifier name: (type_identifier) @name body: (_)) @definition.class
(union_specifier name: (type_identifier) @name body: (_)) @definition.class
(enum_specifier name: (type_identifier) @name body: (_)) @definition.type
(type_definition declarator: (type_identifier) @name) @definition.type
(alias_declaration name: (type_identifier) @name) @definition.type
(namespace_definition name: (namespace_identifier) @name) @definition.module
(preproc_def name: (identifier) @name) @definition.macro
(preproc_function_def name: (identifier) @name) @definition.macro
(enumerator name: (identifier) @name) @definition.constant

(class_specifier name: (type_identifier) @name body: (_)) @scope
(struct_specifier name: (type_identifier) @name body: (_)) @scope
(namespace_definition name: (namespace_identifier) @name body: (_)) @scope

(call_expression function: (identifier) @name) @reference.call
(call_expression
  function: (qualified_identifier name: (identifier) @name)) @reference.call
(call_expression
  function: (field_expression field: (field_identifier) @name)) @reference.call
(call_expression
  function: (template_function name: (identifier) @name)) @reference.call
(call_expression
  function: (qualified_identifier
    name: (template_function name: (identifier) @name))) @reference.call

(preproc_include path: (_) @path) @reference.import
