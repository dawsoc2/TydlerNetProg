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

// Modular exponentiation by repeated squaring
int modexp(int x_, int y, int p) {
    unsigned long long res = 1;
		unsigned long long x = x_ % p; // Need long long to store square of int
 
    while (y > 0) {
        if (y & 1)
            res = (res * x) % p;
        y = y >> 1;
        x = (x * x) % p;  
    }
    return res;
}

// Miller-Rabin test for primality, valid for any odd n >= 3
int isprime(unsigned int n) {
	unsigned int r, d, x;
	unsigned int w, witness[4];
	unsigned int i, j, con;
	if (n < 2047) 
		witness[0] = 2, w = 1;
	else if (n < 1373653) 
		witness[0] = 2, witness[1] = 3,	w = 2;		
	else if (n < 9080191) 
		witness[0] = 31, witness[1] = 73,	w = 2;
	else if (n < 25326001) 
		witness[0] = 2, witness[1] = 3, witness[2] = 5,	w = 3;		
	else if (n < 3215031751) 
		witness[0] = 2, witness[1] = 3, witness[2] = 5, witness[3] = 7,	w = 4;
	else 
		witness[0] = 2, witness[1] = 7, witness[2] = 61, w = 3;	
	
	// Factor n - 1 into 2^r * d
	d = n - 1;
	r = 0;
	while((~d) & 1) {
		d >>= 1;
		r++;
	}
	
	for (i = 0; i < w; i++) {
		con = 0;
		x = modexp(witness[i], d, n);
		if (x == 1 || x == n - 1)
			continue;
		for (j = 0; j < r - 1; j++) {
			x = modexp(x, 2, n);
			if (x == 1)
				return 0;
			if (x == n - 1) {
				con = 1;
				break;
			}
		}
		if (!con)
			return 0;
	}
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
	if (id == 0) pcount++;

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
		if (n >= 3 && isprime(n))
			pcount++;
		if (n < base && n > base - count * 2) {
			MPI_Reduce(&pcount, &primesum, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
			if (id == 0)
				printf("Primes below %d: %d\n", base, primesum);
			base = base*10;
		}
	}

	if (id == 0) {
	   end_time = MPI_Wtime();
	   printf("time elapsed: %.3lf seconds\n", end_time - start_time);
	}

	MPI_Finalize();
	return 0;
}
