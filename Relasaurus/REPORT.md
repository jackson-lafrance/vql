# Env
┌────────────────────┬─────────────────────────────────────────────────────┐
│ Item               │ Value                                               │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Machine            │ Apple M4 Pro                                        │
├────────────────────┼─────────────────────────────────────────────────────┤
│ CPU cores          │ 14                                                  │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Memory             │ 48 GiB                                              │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Operating system   │ macOS 15.6                                          │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Kernel             │ Darwin 24.6.0, ARM64                                │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Language           │ C++20                                               │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Compiler           │ Apple Clang 17.0.0                                  │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Generator language │ Ruby 4.0.0                                          │
├────────────────────┼─────────────────────────────────────────────────────┤
│ Compiler flags     │ -O3 -DNDEBUG -march=native -Wall -Wextra -Wpedantic │
└────────────────────┴─────────────────────────────────────────────────────┘

- I used an optimized build of my guy using 
make -B OPTFLAGS='-O3 -DNDEBUG -march=native' build/relasaurus

# Methods

My Ruby generator makes two relations
R(a b) and S(b, c)

For the main join experiment I used only 1 join per tuple in each relation
where the query was R@{R.b=S.b}S;

I timed right before executing the query and ending it right after before printing out the relation
for perfect measurements

# Nested-loop join measurements
┌────────┬────────┬───────────────┬───────────────┬───────────────┐
│ n      │ m      │ Comparisons   │ Wall time (s) │ Output tuples │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 1,000  │ 1,000  │ 1,000,000     │ 0.0626127     │ 1,000         │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 2,000  │ 2,000  │ 4,000,000     │ 0.248582      │ 2,000         │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 4,000  │ 4,000  │ 16,000,000    │ 0.983917      │ 4,000         │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 8,000  │ 8,000  │ 64,000,000    │ 3.85995       │ 8,000         │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 16,000 │ 16,000 │ 256,000,000   │ 16.2776       │ 16,000        │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 32,000 │ 32,000 │ 1,024,000,000 │ 63.1755       │ 32,000        │
├────────┼────────┼───────────────┼───────────────┼───────────────┤
│ 64,000 │ 64,000 │ 4,096,000,000 │ 251.855       │ 64,000        │
└────────┴────────┴───────────────┴───────────────┴───────────────┘

The measured count is always exactly comparisons = n * m

for relations of the same size its n^2

This is because the nested join loop evaluates every pair
It doesn't stop after finding a match because a tuple could match more than one

# Log-log join analysis
A least-squares regression of log(wall time) against log(n) produced: slope = 1.998170 R^2 = 0.999970

The slope is effectively 2. This means the measured runtime is proportional to approximately n^2

That matches the expected complexity of nested-loop join when both input relations contain n tuples.

Doubling the input size approximately quadrupled the runtime:

┌─────────────────┬────────────┐
│ Transition      │ Time ratio │
├─────────────────┼────────────┤
│ 1,000 → 2,000   │ 3.97       │
├─────────────────┼────────────┤
│ 2,000 → 4,000   │ 3.96       │
├─────────────────┼────────────┤
│ 4,000 → 8,000   │ 3.92       │
├─────────────────┼────────────┤
│ 8,000 → 16,000  │ 4.22       │
├─────────────────┼────────────┤
│ 16,000 → 32,000 │ 3.88       │
├─────────────────┼────────────┤
│ 32,000 → 64,000 │ 3.99       │
└─────────────────┴────────────┘

The small variation makes snes cuz of timing noise, cpu scheduling and memory behaviour

# Selection measurements
I used the query select{a<0}(R);

The generated values of a were all non negative, so it examined every input tuple but returned no output tuples
┌──────────────┬──────────────┬───────────────┬───────────────┐
│ Input tuples │ Examinations │ Wall time (s) │ Output tuples │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 1,000        │ 1,000        │ 0.000070209   │ 0             │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 2,000        │ 2,000        │ 0.000163167   │ 0             │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 4,000        │ 4,000        │ 0.000267417   │ 0             │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 8,000        │ 8,000        │ 0.000692958   │ 0             │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 16,000       │ 16,000       │ 0.00110229    │ 0             │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 32,000       │ 32,000       │ 0.002371      │ 0             │
├──────────────┼──────────────┼───────────────┼───────────────┤
│ 64,000       │ 64,000       │ 0.00522333    │ 0             │
└──────────────┴──────────────┴───────────────┴───────────────┘

The selection counter matched the relation size exactly (n) and the log-log regression made slope = 1.014893
and R^2 = 0.995921

this makes the time complexity about O(n)
The zero output isolates the scanning and predicate evaluation cost because it avoids materializing outputs

# Projection measurements
Query was project{a}(R);

Every generated a value was unique, so projection returned the same number of tuples as the input
┌──────────────┬───────────────┬───────────────┐
│ Input tuples │ Wall time (s) │ Output tuples │
├──────────────┼───────────────┼───────────────┤
│ 1,000        │ 0.000107083   │ 1,000         │
├──────────────┼───────────────┼───────────────┤
│ 2,000        │ 0.00029625    │ 2,000         │
├──────────────┼───────────────┼───────────────┤
│ 4,000        │ 0.000432584   │ 4,000         │
├──────────────┼───────────────┼───────────────┤
│ 8,000        │ 0.000823375   │ 8,000         │
├──────────────┼───────────────┼───────────────┤
│ 16,000       │ 0.00182425    │ 16,000        │
├──────────────┼───────────────┼───────────────┤
│ 32,000       │ 0.00392754    │ 32,000        │
├──────────────┼───────────────┼───────────────┤
│ 64,000       │ 0.00853438    │ 64,000        │
└──────────────┴───────────────┴───────────────┘

The log-log regression made slope = 1.017257 R^2 = 0.992802
Projection was linear because it visited every source tuple, constructed a projected tuple and inserted it

Since I used a hash bucket structure backed by my own tuple hashing and equality implementation, duplicate detection
is just about O(1) since it doesnt have to scan the entire output relation.  This is why this experiment is O(n)

# Comparison of join, selection, and projection
The measured slopes were:
┌────────────┬───────────────┬────────────────────┐
│ Operation  │ Log-log slope │ Approximate growth │
├────────────┼───────────────┼────────────────────┤
│ Join       │ 1.998170      │ Quadratic          │
├────────────┼───────────────┼────────────────────┤
│ Selection  │ 1.014893      │ Linear             │
├────────────┼───────────────┼────────────────────┤
│ Projection │ 1.017257      │ Linear             │
└────────────┴───────────────┴────────────────────┘

Selection and projection just process the tuples each once while join compares every left tuple with every right tuple
The join does 64k more predicate checks than the selection

# Match rate experiment
The join rate at 8k tuples was measured with 3 diff match rates
┌────────────┬─────────────┬───────────────┬───────────────┐
│ Match rate │ Comparisons │ Wall time (s) │ Output tuples │
├────────────┼─────────────┼───────────────┼───────────────┤
│ 0.0        │ 64,000,000  │ 3.88906       │ 0             │
├────────────┼─────────────┼───────────────┼───────────────┤
│ 0.5        │ 64,000,000  │ 3.84636       │ 4,000         │
├────────────┼─────────────┼───────────────┼───────────────┤
│ 1.0        │ 64,000,000  │ 3.85995       │ 8,000         │
└────────────┴─────────────┴───────────────┴───────────────┘

Changing the match rate didn't really do anything because the nested loop join measures every pair
regardless of whether it matches the condition.  The spread is just noise or maybe even a little bit
because making more matches needs extra tuple copying, hashing, and insertion. They don't differ
for me because I hash my tuples for constant time insertion (expected and amortized).

# Prediction for one million tuples per relation
A nested loop join of two one million relations would make 1,000,000 × 1,000,000 = 1,000,000,000,000 comparisons
So since the 64k tuple measurement took 251.855 seconds, the 1mil would take 61,488.037 seconds.

Which is about 17 hour
I did not run this haha

# Making it fast/feasible
A hash join would make this more practical because the engine could build a hash table over S.b, then scan
R and probe the table using each R.b value.  This would make it where building the table takes O(m), but then
checking against it would take O(n), plus the cost of matching the output tuples. This would make it O(n+m+output)
instead of n*m.  This would take more memory and only optimize equality joins while our current implementation lets 
us do all theta join conditions

![Log-log operator performance](stats/performance.svg)
