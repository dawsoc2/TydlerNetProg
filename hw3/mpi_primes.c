#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>

#define LIMIT INT_MAX

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

int main (int argc, char *argv[]) {
	int count, id, n, nsum, pcount, primesum, base;
	double start_time, end_time;
	
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &count);
	MPI_Comm_rank(MPI_COMM_WORLD, &id);

	signal(SIGUSR1, sig_handler);
	
	start_time = MPI_Wtime();
	pcount = 0;

	// consider only odd numbers, thinks 1 is prime and not 2,
	// but doesn't matter for this count.
	// (id - count) exists to allow space for all processes to reduce at 10
	// even if they do not check a number between 1 and 10

	base = 10;
	for (n = ((id - count) * 2) + 1; n <= LIMIT; n += count * 2) {
		if (end_now == 1) {
			MPI_Reduce(&pcount, &primesum, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
			MPI_Reduce(&n, &nsum, 1, MPI_INT, MPI_MAX, 0, MPI_COMM_WORLD);
			if (id == 0)
				printf("Primes below %d: %d\n", nsum, primesum);
			break;
		}
		if (n > 0 && isprime(n))
			pcount++;
		if (n < base && n > base - count * 2) {
			MPI_Reduce(&pcount, &primesum, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
			base = base*10;
			if (id == 0)
				printf("Primes below %d: %d\n", base, primesum);
		}
	}

	if (id == 0) {
	   end_time = MPI_Wtime();
	   printf("time elapsed: %.3lf seconds\n", end_time - start_time);
	}

	MPI_Finalize();
	return 0;
}
