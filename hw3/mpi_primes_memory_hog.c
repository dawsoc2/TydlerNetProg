#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#define LIMIT 100000

int end_now = 0;

void sig_handler(int signo) {
	if (signo == SIGUSR1) {
		end_now = 1;
	}
}

int isprime(int n) {
	int i;
	for (i = 3; i * i <= n; i += 2)
		if ((n % i) == 0)
			return 0;
	return 1;
}

int int_less_than(const void* x, const void* y) {
	const int* nx = x;
	const int* ny = y; 	
	if (*nx > *ny) return 1;
	else return -1;
}

int main (int argc, char *argv[]) {
	int count, id, i, j, intbuffer, primecount, number_of_primes;
	double starttime, endtime;
	
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &count);
	MPI_Comm_rank(MPI_COMM_WORLD, &id);

	if (id == 0) {
		starttime = MPI_Wtime();
		printf("2\n");
		primecount = 1;
		if (count > 1) {
			int* prime_lists[count-1];
			int prime_lists_count[count-1];
			primecount = 1;
			
			//compile each process's list.
			for (i = 1; i < count; i++) {
				MPI_Recv(&intbuffer, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				prime_lists_count[i-1] = intbuffer;
				primecount += intbuffer;
				prime_lists[i-1] = malloc(sizeof(int)*intbuffer);
				MPI_Recv(prime_lists[i-1], intbuffer, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			}
			
			//concatenate total prime_list
			int* prime_list = malloc(sizeof(int)*primecount);
			intbuffer = 0; //keeps track of where we are in prime_list
			for (i = 0; i < (count - 1); i++) {
				for (j = 0; j < prime_lists_count[i]; j++) {
					prime_list[intbuffer] = prime_lists[i][j];
					intbuffer++;
				}
			}
			
			//sort that list
			qsort(prime_list, primecount, sizeof(int), int_less_than);
			
			//print it out
			for (i = 0; i < (intbuffer); i++) {
				printf("%d\n", prime_list[i]);
			}
			
			for (i = 0; i < (count-1); i++) {
				free(prime_lists[i]);
			}
			free(prime_list);
			
		} else { // only one process, don't parallelize
			for (i = 3; i <= LIMIT; i += 2) {
				if (isprime(i)) {
					printf("%d\n", i);
					primecount++;
				}
			}
		}
		endtime = MPI_Wtime();
		printf("found %d primes below %d\n", primecount, LIMIT);
		printf("time elapsed: %.3lf seconds\n", endtime - starttime);
	}
	else {
		int* primes = malloc(sizeof(int)*LIMIT);
		number_of_primes = 0;
		for (i = (id * 2) + 1; i <= LIMIT; i += (count - 1) * 2) {
			if (end_now == 1) {break;}
			if (isprime(i)) {
				primes[number_of_primes] = i;
				number_of_primes++;
			}
		}
		MPI_Send(&number_of_primes, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
		MPI_Send(primes, number_of_primes, MPI_INT, 0, 0, MPI_COMM_WORLD);
		free(primes);
	}

	MPI_Finalize();
}
