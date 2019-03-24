// ~Assignment 4-5~
// Team: Ryan Stillings, Peter Wood
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<stdbool.h>
#include<math.h>
#include<clcg4.h>
#include<mpi.h>
#include<pthread.h>

// #define BGQ 1 // when running BG/Q, comment out when testing on mastiff
#ifdef BGQ
#include<hwi/include/bqc/A2_inlines.h>
#define processor_frequency 1600000000.0 // processing speed for BG/Q
#else
#define GetTimeBase MPI_Wtime
#define processor_frequency 1.0 // 1.0 for mastiff since Wtime measures seconds, not cycles
#endif
#define ALIVE 1
#define DEAD  0

// timing vars
double g_time_in_secs = 0;
unsigned long long g_start_cycles=0;
unsigned long long g_end_cycles=0;
// MPI data
int numRanks = -1; // total number of ranks in the current run
int rank = -1; // our rank
// board/lookup info
int boardSize = 32768; // total # of rows on a square grid
int rowsPerRank = -1; // how many rows each rank is responsible for
int localRow = -1; // which row this rank is responsible for
int globalIndex; // this row's global index

// You define these


/***************************************************************************/
/* Function Decs ***********************************************************/
/***************************************************************************/

// You define these


/***************************************************************************/
/* Function: Main **********************************************************/
/***************************************************************************/

int main(int argc, char *argv[]) {
	// usage check
	if (argc != 1) {
		fprintf(stderr,"Error: %d input argument[s] were supplied, but 0 were expected. Usage: mpirun -np X ./p2p.out\n",argc-1);
		exit(1);
	}

	// init MPI + get size & rank, then calculate game data
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &numRanks);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	rowsPerRank = boardSize/numRanks;
	localRow = rowsPerRank*rank;
	globalIndex = localRow + rowsPerRank * numRanks;
    
	// Init 32,768 RNG streams - each rank has an independent stream
    InitDefault();
    
	// Note, used the mpi_myrank to select which RNG stream to use.
	// You must replace mpi_myrank with the right row being used.
	// This just show you how to call the RNG.
	printf("Rank %d of %d has been started and a first Random Value of %lf\n", rank, numRanks, GenVal(rank));

	MPI_Barrier( MPI_COMM_WORLD );
    
	// Insert your code
    

	// END -Perform a barrier and then leave MPI
    MPI_Barrier( MPI_COMM_WORLD );
    MPI_Finalize();
    return 0;
}

/***************************************************************************/
/* Other Functions - You write as part of the assignment********************/
/***************************************************************************/
