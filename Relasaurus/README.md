# Relasaurus
[REX interpreter](https://github.com/jackson-lafrance/Relasaurus)

Relasaurus is my dinosaur themed relational algebra interpreter. (REX stands for relational expression but also meant to be cool like t-rex)
It uses my custom query language defined in GRAMMAR.md, that is extra special because it completely ignores newlines and spaces in ALL cases!

There are 3.5 main user facing features.

## 1.0) Tree Mode (make run, option 2)

In this mode, you can run arbitrary queries based on my grammar, and you will get back an abstract syntax tree
It prints all pretty with proper indentation as you go down the tree
No queries are actually executed so we don't need any real data

## 2.0) Test Mode (make test)

Runs the required 25 test cases, and prints out their queries, trees, and (hopefully) their pass status

## 3.0) REPL Mode (make run, option 1)

This lets you execute actual queries on real data
This means you must first insert data into memory in the repl, using relation defintions and tuple insertions
These are all defined in GRAMMAR.md so check it out!
Once you have stuff in the "database", you can execute queries on it and these will produce the resulting relations

## 3.5) Stats Mode (make run, option 3)

This is just like REPL mode but you get to see live stats on all queries including comparison counts and query execution times

## 3.5.5) Bonus tools

I also wrote a ruby script that generates relations for the join experiments based on program flags
And a simple python one that plots my data I got from my experiments (Which you should also checkout in my REPORT.MD!!)

# Running instructions
I know I put some instructions there, but basically you can see everything in the makefile


To build the application and test executable

`make`


To build and start the interactive application

`make run`


To run the required test suite

`make test`


To delete compiled files

`make clean`


To make an optimized build that I used for benchmarking run

`make -B OPTFLAGS="-O3 -DNDEBUG -march=native" build/relasaurus`


To run the generator (replace N and M with numbers)

`ruby stats/generate.rb --size=N --match-rate=M | ./build/relasaurus`


To run the python script to make the graph

`python3 stats/plot.py`


also pro tip: :q or :quit quits the repl

# Limitations
The biggest one is that I store everything in memory, so you can't really save any data without alot of tricky piping

Another one is that I only allow numbers and string in the database, which is pretty restrictive so no bools, dates, null values, or anything fancy

Also you can't use SQL you have to use my evil syntax (which I actually think is really good for optimizing against whitespace)

Joins use a nested loop so they need O(n*m) comparisons.  This gets really slow!

I have no query optimizer so expressions are evaluated in the order you specify by the syntax

My bonus programs (for stats generation and plotting) are a little finicky and not very portable

A self join is impossible without a rename because all of the relations would have the same qualified name otherwise, which would error out

