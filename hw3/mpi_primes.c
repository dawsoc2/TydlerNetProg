#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define LIMIT 1000

int isprime(int n) {
	int i;
	for (i = 3; i * i <= n; i += 2)
		if ((n % i) == 0)
			return 0;
	return 1;
}

int main (int argc, char *argv[]) {
	int count, id, n, pcount, primesum;
	double start_time, end_time;
	
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &count);
	MPI_Comm_rank(MPI_COMM_WORLD, &id);

	start_time = MPI_Wtime();
	pcount = 0;

	// consider only odd numbers
	// this prints 1 as a prime instead of 2 lol w/e will fix later
	for (n = (id * 2) + 1; n <= LIMIT; n += count * 2) {
		if (isprime(n)) {
			pcount++;
			printf("%d\n", n);
		}
	}

	MPI_Reduce(&pcount, &primesum, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);

	if (id == 0) {
	   end_time = MPI_Wtime();
	   printf("Primes below %d: %d\n", LIMIT, primesum);
	   printf("time elapsed: %.3lf seconds\n", end_time - start_time);
	}

	MPI_Finalize();
}
