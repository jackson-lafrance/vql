# 5.1 The grammar itself
## 5.1.0 Accepted syntax
In relation defintions and queries, white spaces are ignored, unless within a string.
Strings can be written without quotes, but quotes are needed if you want to include:
- a comma (,),
- a period (.),
- a space ( ),
- a semicolon (;),
- a parantheses (()),
- a brace ({}),
- a single quote character*  ('') (have to use two single quotes right next to eachother to represent one)
- a comment character /* or */
- any binary, unary, or relation defintion/insertion keywords

In tuple-value context, an identifier token represents a bare string
In condition context, an identifier token represents an attribute reference so string constants in conditions must be quoted.
A string that looks numeric must also be quoted.

Newlines are also not allowed within strings and ignored elsewhere
To complete an operation, insert a semicolon (;) at the end of your statement (outside of a list of tuples for a relation definition)
Comments are any text wrapped between /* and */, their contents are ignored when the query/relation is executed

## 5.1.1 Relation definitions
A relation contains a name, a schema (list of attributes where order matters which are name + type (string or number)), and a list of tuples (which follow the schema)
Relations can be defined with or without initial tuples
Since relations are sets duplicate tuples get ignored

### Without any tuples
```cpp
relation Herbavoires (DID = number, Species = string, Extinct = number, Height = number, Name = string,) {};
```


### With tuples
```cpp
relation Herbavoires (DID = number, Species = string, Extinct = number, Height = number, Name = string,) {
    1, Tricerotops, 1, 6.2, Tom;
    1, Tricerotops, 1, 6.2, Tom;
    /* Cool comment */
    2, Brachiosaurus, 1, 5.5, Michael; 
    3, 'Brown Chicken', 0, 1.2, Colin;
};
```

The relation definitions above gets compacted down to the one below once whitespaces, comments, duplicates, and extra newlines are ignored

### Without any values
```cpp
relation Herbavoires(DID=number,Species=string,Extinct=number,Height=number,Name=string,){};
```

### With tuples
```cpp
relation Herbavoires(DID=number,Species=string,Extinct=number,Height=number,Name=string,){1,Tricerotops,1,6.2,Tom;2,Brachiosaurus,1,5.5,Michael;3,'Brown Chicken',0,1.2,Colin;};
```

to add additional tuples to a relation, use the following syntax
```cpp
insert Herbavoires {
    1, Tricerotops, 1, 6.2, Tom;
    3, Pterodactyl, 2, 20, Bob;
};
```

## 5.1.2
The following REX (Relational Expressions) are possible to make in your queries.  A REX is either a relation itself or an expression that returns a relation

### Unary
select{<condition>}(<REX>) -> returns a relation, with the the same schema as the input relation, but only the tuples witch match the condition (conditions with strings in them must be quoted i.e Name='Tom' not Name=Tom)
project{<attribute-list>}(<REX>) -> returns a relation, with the the same tuples (besides dupes that collapse) as the input relation, but only the attributes specified by the attribute list, duplicates of the same attribute collapse to one
renameTable{<new-relation-name>}(<REX>) -> returns a relation, with the the same attributes and tuples as the input relation, but with a new relation name
renameAttribute{<new-attribute-name>, <old-attribute-name>}(<REX>) -> returns a relation, with the the tuples as the input relation, but with one attribute's name changed (and only its name), duplicate attribute names are disallowed

### Binary
<REX>+<REX> -> union, returns a relation, with all of the tuples from both input relations (duplicates collapse).  The relations must have the exact same schema
<REX>&<REX> -> intersect, returns a relation, with only the tuples that exist in both input relations.  The relations must have the exact same schema
<REX>-<REX> -> minus, returns a relation, with only the tuples that exist in the first relation but not in the second.  The relations must have the exact same schema
<REX>*<REX> -> times, returns a relation, with the schema of both relations together (A.name, A.colour, B.name, B.type {where A and B are relation names}), and every tuple of each relation matched with every tuple of the other relation
<REX>@{<condition>}<REX> -> join, returns a relation, performs "times" of the two relations, but then filters the rows based on a "select" condition

Conditions, attribute names/lists, and relation names are all written in braces {}.
Parantheses can group REXs to override associativity and precendence
Relational algebra operations are case insensitive, SELECT is the same as select is the same as sEleCT, but table and attribute names are case sensitive

Conditions are comparisons combined with "&&" (AND), "!" (NOT), "||" (OR), and "()".
The order of evaluation of these conditions (not counting parantheses) is "!", then "&&", then "||"
A comparison has two operands (numbers, strings, and attributes) and one of the operators '=', '!=', '<', '<=', '>', or '>=' between them.
In a comparison, attributes can be qualified or not qualified by relation name (Herbavoires.Name and Name are the same) 
*in join queries you must use qualified attribute names based on the input table

## 5.1.3 Lexical conventions

The tokenizer scans the input from left to right, one character at a time. It uses maximal munch: when several tokens share a prefix, it consumes the longest valid token. For example, >= is one token rather than > followed by =, and && is one token rather than
two & tokens.

Every token records its original text and its starting position in the input. Positions are reported using a zero-based character offset and one-based line and column numbers. The tokenizer also emits an end-of-input token after the final character.

### Whitespace and comments

Spaces, tabs, and newlines are ignored outside quoted strings and newlines are not allowed inside quoted strings

A comment begins with /* and ends at the next */. Reaching the end of the input before finding */ is a lexical error reported at the position of the opening /*

Comment markers appearing inside a quoted string are treated as ordinary string characters.

### Identifiers and keywords

An identifier begins with an ASCII letter and may continue with ASCII letters, digits, or underscores. Identifiers are consumed using maximal munch, so selectValue is one identifier rather than the keyword select followed by the identifier Value.

The reserved words are:

- relation
- insert
- select
- project
- renameTable
- renameAttribute
- string
- number

Reserved words are recognized case-insensitively. For example, select, SELECT, and sElEcT produce the same keyword token. Relation and attribute names remain case-sensitive, and their original spelling is preserved.

Reserved words cannot be used as relation or attribute names. When a reserved word is intended to be a string value, it must be quoted. Words such as union, intersect, and minus are not reserved because this language represents those operators with symbols, so
they may be used as identifiers or bare tuple values.

An identifier token has different meanings in different parser contexts. Inside a tuple, it represents a bare string value. Inside a condition, it represents an attribute reference. Therefore, string constants used in conditions must be quoted.

### Numbers

A number consists of an optional leading -, one or more digits, and an optional fractional part containing a period followed by an arbitrary amount of digits (>= 0)

Examples of valid numbers include 0, 30, -30, 6.2, and -0.5. Forms such as .5, +5, and scientific notation are not part of the language.

When - is immediately followed by a digit, it begins a negative number token. Otherwise, it produces the binary minus token. Therefore, Age>-30 produces the tokens Age, >, and -30, while A-B produces A, -, and B.

A value that looks like a number but is intended to be a string must be quoted.

### Quoted strings

A quoted string begins and ends with a single quote. Characters such as commas, spaces, parentheses, braces, periods, semicolons, operators, and comment markers have no syntactic meaning while inside a quoted string.

Two consecutive single quotes inside a quoted string represent one literal single quote. For example, 'O''Brien' produces the string value O'Brien.

Quoted strings cannot contain newlines. Reaching a newline or the end of the input before finding the closing quote is a lexical error reported at the position of the opening quote.

The quotes are not part of the resulting string value.

### Operators and punctuation

The tokenizer recognizes the following single-character tokens:

- ( and )
- { and }
- ,
- .
- ;
- +
- -
- *
- &
- @
- =
- !
- <
- >

It also recognizes the following two-character tokens using maximal munch:

- !=
- <=
- >=
- &&
- ||

# 5.2 Precedence and associativity
All binary operators have the same precedence
Everything is left associative so in (A+B-C), A+B -> X happens, then X-C happens
Precedence can be overruled by parantheses, the innermost parantheses are evaluated first, etc. So in (A union (B minus C)), B minus C -> X happens, then A union X happens

A little cheat sheet follows
| Domain | Level | Operators or forms | Associativity | Enforcing grammar rule |
| Relational | 2 | `select`, `project`, `renameTable`, `renameAttribute` | Not applicable; their operands are explicitly delimited | `primary = ident \| "(" rex ")" \| uop` |
| Relational | 1 | `+`, `&`, `-`, `*`, `@{condition}` | Left | `rex = primary, { bop }` |
| Condition | 4 | `=`, `!=`, `<`, `<=`, `>`, `>=` | Non-associative | `comparison = operand, cop, operand` |
| Condition | 3 | `!` | Right | `not_expr = "!", not_expr \| "(", condition, ")" \| comparison` |
| Condition | 2 | `&&` | Left | `and_expr = not_expr, { "&&", not_expr }` |
| Condition | 1 | `||` | Left | `or_expr = and_expr, { "||", and_expr }` |


# 5.3 An ambiguity demonstration
Expr ::= Expr "union" Expr
       | Expr "minus" Expr
       | "(" Expr ")"
       | IDENT

A union B minus C
Can be evaluated into the parse trees
         Expr
       /   |   \
    Expr minus Expr
  /  |  \       |
Expr union Expr  C
 |         |
 A         B

         Expr
   /   |   \
Expr union Expr
  |      / | \
 A      /  |  \
      Expr minus Expr
        |         |
        B         C

If A has one tuple ("COOL", "MATH", "GAMES")
and B has one tuple ("COOL", "MATH", "GAMES")
and C has one tuple ("COOL", "MATH", "GAMES")

The first tree will produce an empty relation
The second tree will produce a relation with one tuple ("COOL", "MATH", "GAMES")

My grammar removes the ambiguity because it says the leftmost operation goes first
So only the first parse tree and result will happen
If you want the second one to happen, you have to use parantheses

The rule that does this in my actual grammar is:

rex = primary, { bop }
bop = ("+" | "&" | "-" | "*"), primary | "@", "{", condition, "}", primary


The repetition is folded from left to right. It produces one ordered sequence of primaries and operators instead 
of allowing either operand of every operator to independently become another complete expression.

Therefore A+B-C
is forced to mean (A+B)-C
The other grouping must be requested explicitly A+(B-C)

   The same database instance also demonstrates why the associativity of minus
   matters.

With left associativity:
A-B-C = (A-B)-C = {}-C = {}

With right associativity:
A-(B-C) = A-{} = {("COOL", "MATH", "GAMES")}

My grammar and parser choose the first result because relational binary operators are left-associative

# EBNF

```cpp
input = statement
statement = rel_statement | query_statement

rel_statement = rel_definition | rel_insertion
rel_definition = "relation", ident, "(", schema, ")", "{", { tuple }, "}", ";"
rel_insertion = "insert", ident, "{", { tuple }, "}", ";"

schema = attribute, { ",", attribute }, [ "," ]
attribute = ident, "=", type
type = "string" | "number"

tuple = value, { ",", value }, ";"
value = num | quote_str | ident

query_statement = rex, ";" 
rex = primary, { bop }
bop = ("+" | "&" | "-" | "*"), primary | "@", "{", condition, "}", primary
primary = ident | "(", rex, ")" | uop

uop = "select", "{", condition, "}", "(", rex, ")" | "project", "{", attribute_list, "}", "(", rex, ")" | "renameTable", "{", ident, "}", "(", rex, ")" | "renameAttribute", "{", ident, ",", attribute_ref, "}", "(", rex, ")" ;
attribute_list = attribute_ref, {",", attribute_ref}
attribute_ref = ident, [ ".", ident]

condition = or_expr
or_expr = and_expr, { "||", and_expr }
and_expr = not_expr, { "&&", not_expr }
not_expr = "!", not_expr | "(", condition, ")" | comparison
comparison = operand, cop, operand
cop = "=" | "!=" | "<" | "<=" | ">" | ">="
operand = num | quote_str | attribute_ref

digit = "0" | "1" | "2" | "3" | "4"
      | "5" | "6" | "7" | "8" | "9"

num = [ "-" ], digit, { digit }, [ ".", { digit } ]

quote_str = "'", { character | "''" }, "'"
letter = "A" | "B" | "C" | "D" | "E" | "F" | "G" | "H" | "I" | "J" | "K" | "L" | "M" | "N" | "O" | "P" | "Q" | "R" | "S" | "T" | "U" | "V" | "W" | "X" | "Y" | "Z" |
    "a" | "b" | "c" | "d" | "e" | "f" | "g" | "h" | "i" | "j" | "k" | "l" | "m" | "n" | "o" | "p" | "q" | "r" | "s" | "t" | "u" | "v" | "w" | "x" | "y" | "z"

character = letter | digit | " " | "_" | "+" | "-" | "=" | "<" | ">" | "!" | "@" | "#" | "$" | "%" | "^" | "&" | "*" | "(" | ")" | "{" | "}" | "[" | "]" | "|" | ":" | ";" | "," | "." | "?" | "/" | "`" | "~" | ? backslash ? | ? double_quote ?

ident = letter, { letter | digit | "_" }
```

# 5.4 A parsing strategy justification
I implemented a hand written recursive descent parser because my grammar maps onto separate parsing functions really naturally 
Each major rule has a corresponding function.  Relational binary operators are parsed in a loop inside parse_rex,
which makes them left associative and gives them all the same precedence. 

A recursive descent parser can't use a rule like:
Expr ::= Expr "+" Primary | Primary
or else it will just go on infinitely.  My grammar avoids that though

Boolean precedence is represented by the separate parsing levels of parse_or, which calls parse_and, which calls parse_not, 
which finally parses the comparisons Parenthesied expressions just shoot straight back to parse_or using the overpowered 
std::unique_ptr.  The parser makes and AST represented by a std::variant for the different node types

All the spans from the tokens are passed through so syntax errors can report their actual proper location

I liked this more than a parser generator because the grammar is small and the rules are simple.  So having direct
control over the parsing and error messages was useful for the REPL.

# 5.5 Sources
Geeks for geeks used extensively https://www.geeksforgeeks.org/cpp/switch-statement-in-cpp/
Relational algebra wikipedia https://en.wikipedia.org/wiki/Relational_algebra#Projection
Relational algebra for modern times https://www.relational-algebra.dev/ra-primer/using-joins/
C++ standard library docs  https://en.cppreference.com/cpp/utility/functional/function
Dragon book https://faculty.sist.shanghaitech.edu.cn/faculty/songfu/cav/Dragon-book.pdf
Stack overflow https://stackoverflow.com/questions/50410868/bash-parsing-a-number-out-of-a-text-string, https://stackoverflow.com/questions/15403815/how-to-initialize-the-reference-member-variable-of-a-class, etc
Compile and Run https://www.compilenrun.com/docs/language/cpp/cpp-best-practices/cpp-code-organization/
I can show my tab list if you want to see all the geeks for geeks links and standard library pages

Check design log for areas AI was wrong
