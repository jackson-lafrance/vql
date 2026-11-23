# Design Log

Date: September 14th 
Created a c++ file to implement relational algebra operators.  It was really bad but we just learned about
the assignment and I wanted to practice with it.  This was also my first time using c++, and I wanted to
use it for this project for a challenge and to improve and prepare for my software engineering course.
I used alot of different data structures and alot of AI to make this initial implementation.  It really led
me astray because it convinced me to use template types and Tuples with arbitrary amounts of fields rather
than just leaning on c++ vectors which I didn't know about yet.  You can checkout my old file in the old/ folder

Date: September 16th 
I dove deep and started this project.  I implemented my relation class and tuple representations again.  This time
I did some more exploring in c++ beforehand and actually implemented a relation as a class which holds a list of 
attributes, and a separate unordered map of them for quick O(1) checks, and then also a set of tuples (which will
bite me in the ass later because I forgot I needed my own tuple equality check).  That was another place where
AI got me, because it gave the advice to go with sets I trusted it blindly when it didn't meet our assignments
requirements.

Date: September 17th 
I implemented my Algebra class, and select.  Not much to talk about today but I decided to have algebra
as a kind of abstract class since it doesnt really need to store any information.  Maybe having it as just a
public namespace would have been better, but I was just learning about basic c++ when I decided this and
didn't really know how namespaces worked yet.

Date: September 18th
I finished the rest of the relational algebra operators.  I did all this part without AI's help.  Then after
I had an AI look at it and it told me I should accept everything as const!  This was useful actually because
I was just returning new relations anyways so getting them as const inputs helped ensure safety that I wasn't
mutating the originals.  This ended up helping along the way cuz it made nested expressions very natural because
the output of every operator is a Relation, which can then be the input for another operator.

Date September 20th
I decided to separate out my Schema into its own class.  Also this was the point that I realized my std::variant
of integer, double, and std::string should just be doubles and strings because the assignment spec just wanted
number.  Notably when I ran my initial stuff through ai against the assignment spec it did not point this out and
I only realized later.  Anyways redesigning the schema into its own class was helpful because it separated
the responsibilities of tuple storage from the schema stuff like matching tuples to a schema and comparing schemas.
This made all my projection, and later attribute resolution and parser output stuff easier to do.

Date: September 22nd-23rd
This was when I designed my grammar for the most part.  The biggest decision I made early on
was that I didn't want any whitespace or newlines in my grammar to matter.  This made the lexer really easy
to implement later.  I decided some other stuff like keywords being case insensitive and the statement ending
after the last ; when all the braces match up.  I decided to make my grammar left associative and all the binary
operators have equal precendence.  Logically though I used the standard ! then && then || precendence.  I wrote
the grammar first here so the lexer and parser would be easier.  AI slipped me up here because it said I should
change my grammar and have a program be { statement }, which I ended up reverting because all my queries are run
one at a time

Date September 24th-25th
I didn't code anything these days but I read chapters 2 and 4 of the dragon book.  These were really helpful
because I was scared about writing a tokenizer and a parser because I didn't have a clue what the fuck
an abstract syntax tree was or how I would even tokenize things.  And these chapters even though they were
about compilers made it alot easier to understand.

Date: September 27th
Today I designed and implemented my lexer.  One important decision I made was to have my lexer track a complete
source span for each token.  This made error handling really easy later down the line.  I used a peek() function
so that I could use maximal munch, which is the coolest name ever, to get my operators like >=, !=, &&.  I really
didn't know how I was gonna implement this before I started because I've only ever really parsed strings using
regular expressions, but now I feel like I could explain it to anyone!  My lexer returns tokens that all have 
a specific span, token type, and lexeme.

Date September 28th
I built out the recursive descent parser today. I basically started by making all these struct types, which
I could basically rip straight from my grammar almost.  It uses a cool c++ feature called std::unique_ptr
which ai helped me find that basically lets a pointer own an object.  This was helpful for recursive condition
or rex parsing.  It builds left associative relational expressions and repeatedly replaces the left expression.
My grammar makes this possible without infinite loops!

Date September 28th
After that in the night time I implemented the start of my interpreter, which basically executes the AST that the
parser creates.  It also holds all the relations in its memory which makes sense because its the one manipulating
them.  This was when I realized alot of problems that trusting AI a little too closely got me.  One was that
qualified names in the joins could not be repeat joined, I had to fix this by just having them keep their initial name.
You can always get around this since I made a separate rename algebra function for specifically attributes.  Another
thing I realized was that my join was initially just a times + a select, which was WAY too slow.  So I made a direct
nested loop implementation.  This made comparison counting explicit and avoided making a whole cartesian product when
I was just gonna keep parts of it anyways

Date: September 29th
I finished up the interpreter today and fixed alot more bugs to the point where I could run a preset string through
the whole pipeline and there would be a relation on the other side (no way to print it yet).  So I added proper relation
printing and then worked on making a proper repl, which was harder than you would think, to execute queries in.  Then
I used ai to modify my repl into a specific version just to print out trees but it was awful and way too overcomplicated
of a pretty printing frontend for a database project, so I remade it myself

Date: September 30th
This was the final day and I grinded out a bunch more bug fixes.  Including the dreaded illegal set of tuples in my
relation.  To fix this I basically implemented my own hashmap, where I can has my own tuples based on their values
using some magic numbers I found online, and two tuples with the same items in them will always hash to the same
hashcode.  This lets me use an unordered map to store them and sped up alot of my algebra operations and also
matched the assignment spec of not using built in deduplication.  Whoops!  Then I worked on my stats gathering
in my cpp code using a libray for benchmarking, and made a ruby script to generate the data (ruby is my favourite 
language) and python matplotlib guy for the data (i hate python).  Then I tried to run my ruby script,
and realized it would take 2 hours to run 64k tuples.  I tried for a while a bunch of ai solutions to fix this,
but they all sucked and eventually I found a stack overflow thread that showed how to optimize my c++ compilation.
Which extremely sped up the process and made it only take a few minutes instead.
