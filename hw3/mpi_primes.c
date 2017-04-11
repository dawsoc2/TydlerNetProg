#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define LIMIT 100000

int isprime(int n) {
	int i;
	for (i = 3; i * i <= n; i += 2)
		if ((n % i) == 0)
			return 0;
	return 1;
}

int main (int argc, char *argv[]) {
	int count, id, i, intbuffer, source, primecount;
	double starttime, endtime;
	
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &count);
	MPI_Comm_rank(MPI_COMM_WORLD, &id);

	if (id == 0) {
		starttime = MPI_Wtime();
		printf("2\n");
		primecount = 1;
		if (count > 1) {
			for (i = 3; i <= LIMIT; i += 2) {
				source = (i/2 - 1) % (count-1) + 1; // awkward conversion, but works
				MPI_Recv(&intbuffer, 1, MPI_INT, source, 0,
					MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				if (intbuffer) {
					printf("%d\n", i);
					primecount++;
				}
			}
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
		for (i = (id * 2) + 1; i <= LIMIT; i += (count - 1) * 2) {
			intbuffer = isprime(i);
			MPI_Send(&intbuffer, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
		}
	}

	MPI_Finalize();
}

