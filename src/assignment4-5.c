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
#define DEBUG true

#define ALIVE 1
#define DEAD  0

// MPI data
int numRanks = -1; // total number of ranks in the current run
int rank = -1; // our rank
// game/board info
#define boardSize 32768 // total # of rows on a square grid
int numThreads = -1; // total number of threads at each rank (including the rank itself, meaning we spawn numThreads-1 pthreads); passed in as arg1
pthread_barrier_t threadBarrier;
int numTicks = -1; // how many ticks of the simulation to perform; passed in as arg2
bool** boardData;
bool ghostTop[boardSize];
bool ghostBot[boardSize];
int rowsPerRank = -1; // how many rows each rank is responsible for
int rowsPerThread = -1; // how many rows each thread is responsible for
int localRow = -1; // which row this rank is responsible for
int globalIndex; // this row's global index
// timing/stats
double g_time_in_secs = 0;
unsigned long long g_start_cycles=0;
unsigned long long* liveCellCounts; //array of ints corresponding to the # of live cells at the end of each simulation tick
unsigned long long* totalLiveCellCounts; //live cell counts across all ranks combined
pthread_mutex_t counterMutex = PTHREAD_MUTEX_INITIALIZER; // lock used to ensure safe thread counting

/**
 * run the simulation for numTicks steps. Called by each thread on each rank.
 * @param threadNum: an int containing the calling thread's index on its local rank
 */
void *runSimulation(void* threadNum) {
	int threadId = *((int*)threadNum);
	for (int i = 0; i < numTicks; ++i) {
		//exchange row data
		if (threadNum == 0) {
			//TODO: data exchange
		}
		//sync threads on the fresh tick data
		pthread_barrier_wait(&threadBarrier);
		// TODO: game of life logic
		// update live cell count
		unsigned long long localLiveCells = 0;
		for (int k = rowsPerThread*threadId; k < rowsPerThread*(threadId+1); ++k) {
			for (int r = 0; r < boardSize; ++r) {
				localLiveCells += boardData[k][r];
				// TODO: this local sum operation is running more slowly than expected; further consideration should be given
			}
		}
		// grab the counter lock for the increment critical section
		while (pthread_mutex_trylock(&counterMutex) != 0);
		liveCellCounts[i]+=localLiveCells;
		pthread_mutex_unlock(&counterMutex);
		if (DEBUG && rank == 0 && threadId == 0) printf("%d\n",i);

	}
	//all done with thread barriers
	return NULL;
}


int main(int argc, char *argv[]) {
	// usage check
	if (argc != 3) {
		fprintf(stderr,"Error: %d input argument[s] were supplied, but 2 were expected. Usage: mpirun -np X ./a.o numThreadsPerRank numTicks\n",argc-1);
		exit(1);
	}
	numThreads = atoi(argv[1]);
	numTicks = atoi(argv[2]);

	//use a pthread barrier to ensure threads don't tick on stale data
	pthread_barrier_init(&threadBarrier, NULL, numThreads);

	// init MPI + get size & rank, then calculate board data
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &numRanks);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	rowsPerRank = boardSize/numRanks;
	rowsPerThread = rowsPerRank/numThreads;
	localRow = rowsPerRank*rank;
	globalIndex = localRow + rowsPerRank * numRanks;  // algorithm provided by the assignment doc
	liveCellCounts = calloc(numTicks, sizeof(unsigned long long));
	if (rank == 0) totalLiveCellCounts = calloc(numTicks, sizeof(unsigned long long));

	if (DEBUG) printf("%d: numRanks: %d, numThreads: %d, rowsPerRank: %d, rowsPerThread: %d\n",rank, numRanks,numThreads, rowsPerRank, rowsPerThread);

	// Init 32,768 RNG streams - each rank has an independent stream
	InitDefault();
	MPI_Barrier( MPI_COMM_WORLD );
	g_start_cycles = GetTimeBase();

	// allocate local board chunk and initialize universe to ALIVE
	boardData = malloc(rowsPerRank * sizeof(bool*));
    for (int i = 0; i < rowsPerRank; ++i) {
    	boardData[i] = malloc(boardSize * sizeof(bool));
    	for (int r = 0; r < boardSize; boardData[i][r++] = ALIVE);
    }
    // init ghost rows
    for (int i = 0; i < boardSize; ++i) {
    	ghostTop[i] = ALIVE;
    	ghostBot[i] = ALIVE;
    }

    // construct pthreads to run the main simulation
    pthread_t threads[numThreads];
    int threadIds[numThreads];
    for (int i = 1; i < numThreads; ++i) {
    	threadIds[i] = i;
    	pthread_create(&threads[i], NULL, runSimulation, &threadIds[i]);
    }

    // run the simulation on the root process as well, who gets treated as pthread #0
    threadIds[0] = 0;
    runSimulation(&threadIds[0]);
    // cleanup pthreads and thread barrier
    for (int i = 1; i < numThreads; pthread_join(threads[i++], NULL));
    pthread_barrier_destroy(&threadBarrier);
    // sum the live cell counts across all ranks
    MPI_Reduce(liveCellCounts, totalLiveCellCounts, numTicks, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);

    // timing and analysis
    double time_in_secs = (GetTimeBase() - g_start_cycles) / processor_frequency;
    printf("rank %d: Elapsed time = %fs\n",rank,time_in_secs);
    if (rank == 0) printf("rank %d: totalLiveCellCounts[3]=%llu\n",rank, totalLiveCellCounts[3]);

	// END -Perform a barrier and then leave MPI
    MPI_Barrier( MPI_COMM_WORLD );
    MPI_Finalize();

    // cleanup dynamic memory
    free(liveCellCounts);
    free(totalLiveCellCounts);
    for (int i = 0; i < rowsPerRank; free(boardData[i++]));
    free(boardData);

    return 0;
}
