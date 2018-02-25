#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "util.h"
#include "queue.h"
#include <pthread.h>
#include "multi-lookup.h"

#define MINARGS 3
#define USAGE "<inputFilePath> <outputFilePath>"
#define SBUFSIZE 1025
#define INPUTFS "%1024s"

//declare global variables that threads need access to
int inFileCount;	//number of input files
int inFilesRead = 0;	//number of input files processed
queue q;		//global queue for domain names to be managed
FILE* outputfp;		//association to output file

//synchronizing needs
pthread_mutex_t qLock;		//synch access to the queue
pthread_mutex_t outFileLock;	//synch access to the output file
pthread_mutex_t readCountLock;	//synch access to the counter for processed files
pthread_cond_t pushedQ;		//conditional for when resolving threads are waiting for requesting threads to push to queue

//routine for requesting threads to run
void* reqRoutine(void* filename){
	char hostname[SBUFSIZE];			//holds hostname scanned from file
	char* payload;					//pointer to point to copy of hostname allocated in heap memory
	FILE* inFile = fopen((char*) filename, "r");	//open input file passed to thread for reading; have to recast filename into char*
	if(!inFile){
		perror("Input file open error");
		pthread_exit(NULL);
	}
	while(fscanf(inFile, INPUTFS, hostname) > 0){	//start scanning input file hostname by hostname
		int done = 0;				//pseudo condition variable that ensures each hostname makes its way onto the queue
		while(!done){
			pthread_mutex_lock(&qLock);			//acquire the queue lock
			if(!queue_is_full(&q)){				//check if queue is full
				payload = malloc(SBUFSIZE);		//allocate memory in heap memory for payload
				strncpy(payload, hostname, SBUFSIZE);	//copy hostname to payload; the hostname needs to be placed in heap to work with queue and to be accessed by other threads
				queue_push(&q, (void*) payload);
				done = 1;				//hostname was added to queue so now done
				pthread_cond_signal(&pushedQ);		//signal resolver threads if any were waiting for hostnames to be pushed to queue
			}
			pthread_mutex_unlock(&qLock);			//release queue lock
			if(!done){			//if done still equals 0 (i.e. queue was full)
				usleep(rand() % 100);	//sleep for a bit then try again
			}
		}
	}
	pthread_mutex_lock(&readCountLock);	//when here, file has been scanned; acquire lock for the processed file counter
	inFilesRead++;				//add 1 to counter
	pthread_mutex_unlock(&readCountLock);	//release
	fclose(inFile);				//close the input file
	return NULL;
}

//routine for resolver threads to run
void* resRoutine(){
	char IP[INET6_ADDRSTRLEN];
	while(1){				//infinite loop b/c these threads should always be checking queue for hostnames; there is only one exit condition
		pthread_mutex_lock(&qLock);	//acquire queue lock
		while(queue_is_empty(&q)){	//if queue is empty, either waiting on req threads to push to queue, or all files have been processed and work is done
			pthread_mutex_lock(&readCountLock);		//first check if all files have been processed
			int done = (inFileCount == inFilesRead);	//work is done if the number of input files equals the number of files processed; this is the only exit condition for the thread
			pthread_mutex_unlock(&readCountLock);
			if(done){					//if work is done, release queue lock and exit
				pthread_mutex_unlock(&qLock);
				return NULL;
			}
			else{						//else, wait for a requester thread to push to the queue
				pthread_cond_wait(&pushedQ, &qLock);	//use condition variable; this waits for signal from a req thread and also releases the queue lock
			}						//upon signal, queue lock is auto-reacquired
		}							//at this point, work is not done, and there are entries in the queue that need to be handled
		char* hostname = (char*) queue_pop(&q);			//take hostname off of queue
		pthread_mutex_unlock(&qLock);				//done with queue so release queue lock
		int error = dnslookup(hostname, IP, sizeof(IP));	//use dnslookup utility to find IP of hostname taken from queue
		if(error == UTIL_FAILURE){				//handle any errors
			strncpy(IP, "", sizeof(IP));
			fprintf(stderr, "Bogus hostname: %s\n", hostname);
		}
		pthread_mutex_lock(&outFileLock);		//synch access to output file
		fprintf(outputfp, "%s, %s\n", hostname, IP);	//now print hostname and its IP to the out file
		pthread_mutex_unlock(&outFileLock);		//release lock on out file
		free(hostname);					//now that we're done with the hostname, we can free the heap memory it was using
	}							//routine continues to repeat until the exit condition is reached
}

int main(int argc, char* argv[]){

	//check number of arguments against lower limit
	if(argc < MINARGS){
		fprintf(stderr, "Not enough arguments: %d\n", (argc - 1));
		fprintf(stderr, "Usage:\n %s %s\n", argv[0], USAGE);
		return EXIT_FAILURE;
	}

	//determine number of input files to process and check against upper limit
	inFileCount = argc - 2;
	if(inFileCount > 10){
		fprintf(stderr, "Input files exceed limit of 10\n");
		return EXIT_FAILURE;
	}
	
	//open the output file for threads to write to
	outputfp = fopen(argv[(argc-1)], "w");
	if(!outputfp){
		perror("Output file open error");
		return EXIT_FAILURE;
	}
	
	//initialize queue
	if(queue_init(&q, 0) == -1){
		fprintf(stderr, "Queue build failure\n");
		return EXIT_FAILURE;
	}

	//populate an array with input file names
	char* inFileNames[inFileCount];
	for(int i = 1; i < argc - 1; i++){	//input files start at second index of argv[] and end at second to last index of argv[]
		inFileNames[i - 1] = argv[i];
	}

	//initialize synch variables
	pthread_mutex_init(&qLock, NULL);
	pthread_mutex_init(&outFileLock, NULL);
	pthread_mutex_init(&readCountLock, NULL);
	pthread_cond_init(&pushedQ, NULL);

	//create requesting thread for each input file
	pthread_t reqThreads[inFileCount];
	int reqError;
	int j;
	for(j = 0; j < inFileCount; j++){
		reqError = pthread_create(&reqThreads[j], NULL, reqRoutine, (void *) inFileNames[j]);	//pass corresponding filename as argument for thread routine
		if(reqError){
			fprintf(stderr, "Requester thread create error: %d\n", reqError);
			exit(EXIT_FAILURE);
		}
	}

	//create resolver threads equal to number of cores
	int coreCount = sysconf(_SC_NPROCESSORS_ONLN);
	pthread_t resThreads[coreCount];
	int resError;
	int k;
	for(k = 0; k < coreCount; k++){
		resError = pthread_create(&resThreads[k], NULL, resRoutine, NULL);
		if(resError){
                        fprintf(stderr, "Resolver thread create error: %d\n", resError);
                        exit(EXIT_FAILURE);
		}
	}

	//join all threads/wait for routines to finish
	for(j = 0; j < inFileCount; j++){
		pthread_join(reqThreads[j], NULL);
	}

	for(k = 0; k < coreCount; k++){
		pthread_join(resThreads[k], NULL);
	}

	//close output file and clean up
	fclose(outputfp);
	queue_cleanup(&q);
	pthread_mutex_destroy(&qLock);
	pthread_mutex_destroy(&outFileLock);
	pthread_mutex_destroy(&readCountLock);
	pthread_cond_destroy(&pushedQ);

	return EXIT_SUCCESS;
}
