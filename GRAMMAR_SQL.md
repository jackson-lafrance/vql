# SQL grammar
## Lexical rules
- keywords ignore case
- identifiers preserve case and match a letter or underscore followed by letters, digits, or underscores
- integers have decimal digits and an optional adjacent `+` or `-` 
- they gotta fit signed 32-bit integers
- strings use single quotes. `''` inside a string is a quote thats a part of it
- raw newlines in strings are rejected, just like in my other project
- unlike in relasaurus though, spaces, tabs, carriage returns, and newlines separate tokens 
- `--` starts a comment that ends at the next carriage return, newline, or end of input
- no REX operators from my other project are accepted just SQL

## EBNF
```ebnf
script            = { statement, ";" } ;
statement         = create | insert | query ;

create            = "CREATE", "TABLE", ident, "(", coldef,
                    { ",", coldef }, ")" ;
coldef            = ident, ( "INT" | "VARCHAR", "(", integer, ")" ) ;
insert            = "INSERT", "INTO", ident, "VALUES", "(", literal,
                    { ",", literal }, ")" ;
literal           = integer | string ;

query             = query_primary, { setop, query_primary } ;
query_primary     = select_block | "(", query, ")" ;
setop             = "UNION", [ "ALL" ] | "INTERSECT" | "EXCEPT" ;
select_block      = "SELECT", [ "DISTINCT" ], select_list,
                    "FROM", from_list,
                    [ "WHERE", condition ],
                    [ "GROUP", "BY", col_list, [ "HAVING", condition ] ],
                    [ "ORDER", "BY", sort_list ],
                    [ "LIMIT", integer ] ;
select_list       = "*" | select_item, { ",", select_item } ;
select_item       = ( column | aggregate ), [ "AS", ident ] ;
aggregate         = ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" ),
                    "(", ( column | "*" ), ")" ;
from_list         = table_ref, { ",", table_ref }
                  | table_ref, "JOIN", table_ref, "ON", condition,
                    { "JOIN", table_ref, "ON", condition } ;
table_ref         = ident, [ "AS", ident ]
                  | "(", query, ")", "AS", ident ;
col_list          = column, { ",", column } ;
sort_list         = sort_key, { ",", sort_key } ;
sort_key          = column, [ "ASC" | "DESC" ] ;
column            = ident, [ ".", ident ] ;

condition         = disjunction ;
disjunction       = conjunction, { "OR", conjunction } ;
conjunction       = negation, { "AND", negation } ;
negation          = "NOT", negation | "(", condition, ")" | comparison ;
comparison        = operand, comparison_op, operand ;
comparison_op     = "=" | "<>" | "<" | "<=" | ">" | ">=" ;
operand           = column | literal | aggregate ;
```

## Precedence and associativity

| Context | Tightest to loosest (pause) | Associativity |
| --- | --- | --- |
| Conditions | Comparison, NOT, AND, OR | Comparison: none NOT: right AND/OR: left |
| Queries | Parenthesized query then all set operators equally | Set operators: left |

so `NOT a = 1 AND b = 2 OR c = 3` means
`((NOT (a = 1)) AND (b = 2)) OR (c = 3)`.
`A UNION B INTERSECT C` means `(A UNION B) INTERSECT C`
these make left associated trees which is good for my parser

ORDER BY and LIMIT belong to individual select blocks not to an entire set operation
Comma FROM lists and JOIN chains cannot be mixed in one FROM clause

## Differences from what the spec said
- **Aliases always require AS** 
- **Aggregate operands are included** The spec doesnt have them but the required `HAVING COUNT(*) > 5` example needs them
- Syntax accepts all aggregate function/star combinations. my semantic analyzer is gonna be what rejects combinations like `SUM(*)`
- Aggregates without GROUP BY are allowed to be in the select list
