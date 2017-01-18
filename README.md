This is a fork rebased in version 3.8.1.

Extending Cachegrind to include L2 cache information and TLB simulation. The former involved extending the current cache information to include L2 cache when 3 levels of cache are present, while the latter involved the design and implementation of a complete Intel x86 TLB simulator that provides a) the number of hits and misses, b) the pages used and the times used and c) per file, per function and, per source code line statistics.

See more details at http://opus.bath.ac.uk/39762/ or directly get the pdf file from [here](http://opus.bath.ac.uk/39762/1/kaparelos_sk_dissertation_2014_04.pdf).
