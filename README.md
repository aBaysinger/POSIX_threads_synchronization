# POSIX_threads_synchronization
Example of resource synchronization when using multiple threads

This C program takes multiple files of domain names and pairs them with their IP addresses using multithreading.

multi-lookup.c is the driving program and uses a queue, output file and counter variables as shared resources between requesting and resolving threads.
Mutex locks are used to synchronize access to the shared resources.
Requesting threads push domain names to the queue while resolver threads pop names from the queue and find their IP's.

After all input files are read, the result is a single text file with each domain name paired with its IP.

This was an assignment focused on resource access synchronization and the complexities of multithreading.
queue.c and util.c were provided helper programs and were not written by me.

This program must be run on UNIX systems. It does work on Windows Subsystem for Linux (WSL).

From the command line:
1. To compile, run "make"
2. To run: ./multi-lookup names1.txt names2.txt names3.txt names4.txt names5.txt results.txt
3. To clean and remove results file, run "make clean"
