grammar CidxSignature;

// CIDX canonical signature serialization, version 1.
// Names and types are quoted canonical UTF-8 NFC strings. The serializer
// removes parameter names, default arguments, comments, and formatting space.

signature
    : name LPAREN parameterList RPAREN cvRefSpec? noexceptSpec?
      templateSpec? constraintSpec? returnSpec? EOF
    ;

name
    : STRING templateArguments?
    ;

templateArguments
    : LT templateArgument (COMMA templateArgument)* GT
    ;

templateArgument
    : TYPE_PARAMETER STRING
    | NON_TYPE_PARAMETER STRING
    | TEMPLATE_PARAMETER STRING
    ;

parameterList
    : (canonicalType (COMMA canonicalType)*)?
    ;

canonicalType
    : STRING
    ;

cvRefSpec
    : CONST VOLATILE? refQualifier?
    | VOLATILE refQualifier?
    | refQualifier
    ;

refQualifier
    : AMP
    | AMPAMP
    ;

noexceptSpec
    : NOEXCEPT (LPAREN STRING RPAREN)?
    ;

templateSpec
    : TEMPLATE LT templateParameter (COMMA templateParameter)* GT
    ;

templateParameter
    : TYPE_PARAMETER STRING
    | NON_TYPE_PARAMETER STRING
    | TEMPLATE_PARAMETER STRING
    ;

constraintSpec
    : REQUIRES STRING
    ;

returnSpec
    : ARROW canonicalType
    ;

CONST               : 'const';
VOLATILE            : 'volatile';
NOEXCEPT            : 'noexcept';
TEMPLATE            : 'template';
TYPE_PARAMETER      : 'type';
NON_TYPE_PARAMETER  : 'non_type';
TEMPLATE_PARAMETER  : 'template_parameter';
REQUIRES            : 'requires';
LPAREN              : '(';
RPAREN              : ')';
LT                  : '<';
GT                  : '>';
COMMA               : ',';
AMPAMP              : '&&';
AMP                 : '&';
ARROW               : '->';
STRING              : '"' ( '\\"' | '\\\\' | ~["\\\u0000-\u001F] )* '"';
WS                  : [ \t\r\n]+ -> skip;
