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
#define DEBUG false
#define BOARDTESTING true

#define ALIVE 1
#define DEAD  0

// MPI data
int numRanks = -1; // total number of ranks in the current run
int rank = -1; // our rank
// game/board info
int threshold; // probability that cells will randomize
#define boardSize 32 // total # of rows on a square grid
int numThreads = -1; // total number of threads at each rank (including the rank itself, meaning we spawn numThreads-1 pthreads); passed in as arg1
pthread_barrier_t threadBarrier;
int numTicks = -1; // how many ticks of the simulation to perform; passed in as arg2
bool** boardData;
int** heatmap;
bool* ghostTop;
bool* ghostBot;
int rowsPerRank = -1; // how many rows each rank is responsible for
int rowsPerThread = -1; // how many rows each thread is responsible for
int globalIndex; // this row's global index
// timing/stats
double g_time_in_secs = 0;
unsigned long long g_start_cycles=0;
unsigned int* liveCellCounts; //array of ints corresponding to the # of live cells at the end of each simulation tick
unsigned int* totalLiveCellCounts; //live cell counts across all ranks combined
pthread_mutex_t counterMutex = PTHREAD_MUTEX_INITIALIZER; // lock used to ensure safe thread counting

/*
int countNeighbors(int x, int y){
	int n = 0;
	for(int i=-1; i<=1; ++i){
		for(int j=-1; j<=1; ++j){
			// Count the cells in the 3x3 area centered on (x,y) that are alive, but skip (x,y). If y would be out of bounds, use the ghost rows.
			// To prevent x from going out of bounds, make it wrap around, so that -1 becomes boardSize - 1.
			if(i!=0 || j!=0) n += ( ( (y+j==-1) ? ghostBot : ((y+j==rowsPerRank) ? ghostTop : boardData[y+j]) )[(boardSize+x+i)%boardSize] == ALIVE );
		}
	}
	return n;
}
*/

/**
 * return the number of live neighbors of the cell at the specified row/col, wrapping around at the edges of the board
 * @param row: the row at which the desired cell resides
 * @param col: the column at which the desired cell resides
 * @return: the live neighbor count at the specified cell
 */
int countNeighbors(int row, int col) {
	//by storing pointers to the top and bottom rows and the left and right column positions up-front, this routine should be very efficient
	bool* botRow = (row == rowsPerRank-1) ? ghostBot : boardData[row+1];
	bool* topRow = (row == 0) ? ghostTop : boardData[row-1];
	int leftCol = (col == 0) ? boardSize-1 : col-1;
	int rightCol = (col == boardSize-1) ? 0 : col+1;
	//     topLeft         + topCenter   + topRight         + centerLeft              + centerRight              + bottomLeft      + bottomCenter+ bottomRight
	return topRow[leftCol] + topRow[col] + topRow[rightCol] + boardData[row][leftCol] + boardData[row][rightCol] + botRow[leftCol] + botRow[col] + botRow[rightCol];
}

// Copies the current tick data into the cumulative heatmap.
void updateHeatmap() { 
	for (int i = 0; i < rowsPerRank; ++i){
		for (int j = 0; j < boardSize; ++j){
			heatmap[i][j] += boardData[i][j];
		}
	}
}

/**
 * display the board state for debugging purposes
 */
void printBoard() {
	for (int k = 0; k<rowsPerRank; ++k) {
		for (int r = 0; r < boardSize; ++r) {
		printf(boardData[k][r]?"x":"_");
		}
		printf("\n");
	}
	if (rank == numRanks-1) printf("\n");
	fflush(stdout);
}

void printHeatmap(){
	if(rank != 0) MPI_Recv(0, 0, 0, rank-1, HEATMAP_SYNC_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	
	for (int k = 0; k<rowsPerRank; ++k) {
		for (int r = 0; r < boardSize; ++r) {
			printf("%d ", heatmap[k][r]);
		}
		printf("\n");
	}

	if(rank != numRanks-1) MPI_Send(0, 0, 0, rank+1, HEATMAP_SYNC_TAG, MPI_COMM_WORLD);
	fflush(stdout);
}

/**
 * run the simulation for numTicks steps. Called by each thread on each rank.
 * @param threadNum: an int containing the calling thread's index on its local rank
 */

void *runSimulation(void* threadNum) {
	int threadId = *((int*)threadNum);
	MPI_Request sReqTop, sReqBot, rReqTop, rReqBot;
	MPI_Status status;
	if (BOARDTESTING && threadId == 0) printBoard();
	for (int i = 0; i < numTicks; ++i) {
		//exchange row data with top and bottom neighbors
		if (numRanks > 1 && threadId == 0) {
			//send top
			MPI_Isend(boardData[rowsPerRank-1], boardSize, MPI_C_BOOL, rank==numRanks-1?0:rank+1, 0, MPI_COMM_WORLD, &sReqTop);
			//send bottom
			MPI_Isend(boardData[0], boardSize, MPI_C_BOOL, rank==0?numRanks-1:rank-1, 0, MPI_COMM_WORLD, &sReqBot);
			//receive top
			MPI_Irecv(ghostBot, boardSize, MPI_C_BOOL, rank==numRanks-1?0:rank+1, 0, MPI_COMM_WORLD, &rReqTop);
			//receive bottom
			MPI_Irecv(ghostTop, boardSize, MPI_C_BOOL, rank==0?numRanks-1:rank-1, 0, MPI_COMM_WORLD, &rReqBot);
			//make sure we've received the ghost data before proceeding
			MPI_Wait(&rReqTop, &status);
			MPI_Wait(&rReqBot, &status);
		}
		//sync threads on the current tick after we've received ghost rows
		pthread_barrier_wait(&threadBarrier);
		// update grid
		unsigned int localLiveCells = 0;
		for (int k = rowsPerThread*threadId; k < rowsPerThread*(threadId+1); ++k) {
			for (int r = 0; r < boardSize; ++r) {
				// consult rng and count neighbors to determine cell state
				double random = GenVal(k+globalIndex)*100;  // shift rng range from 0-1 to 0-100
				// when we don't reach the random threshold, treat >= threshold/2 as false, and < threshold/2 as true
				if (random < threshold) boardData[k][r] = ( random < (threshold>>2) );
				else {
					int neighbors = countNeighbors(k,r);
					boardData[k][r] = neighbors == 3 || (neighbors == 2 && boardData[k][r] == ALIVE);
				}
				//keep track of local live cells
				localLiveCells += boardData[k][r];
			}
		}
		// grab the counter lock for the increment critical section
		while (pthread_mutex_trylock(&counterMutex) != 0);
		liveCellCounts[i]+=localLiveCells;
		pthread_mutex_unlock(&counterMutex);
		if (DEBUG && rank == 0 && threadId == 0) printf("%d\n",i);
		if (BOARDTESTING && threadId == 0) printBoard();
		updateHeatmap();

	}
	printHeatmap();
	//all done with thread barriers
	return NULL;
}


int main(int argc, char *argv[]) {
	// usage check
	if (argc != 4) {
		fprintf(stderr,"Error: %d input argument[s] were supplied, but 3 were expected. Usage: mpirun -np X ./a.o numThreadsPerRank numTicks threshold\n",argc-1);
		exit(1);
	}
	//grab run parameters from cmd args
	numThreads = atoi(argv[1]);
	numTicks = atoi(argv[2]);
	threshold = atof(argv[3]);

	//use a pthread barrier to ensure threads don't tick on stale data
	pthread_barrier_init(&threadBarrier, NULL, numThreads);

	// init MPI + get size & rank, then calculate board data
	MPI_Init(&argc, &argv);
	MPI_Comm_size(MPI_COMM_WORLD, &numRanks);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	rowsPerRank = boardSize/numRanks;
	rowsPerThread = rowsPerRank/numThreads;
	globalIndex = rowsPerRank * rank;  // algorithm provided by the assignment doc
	liveCellCounts = calloc(numTicks, sizeof(unsigned int));
	if (rank == 0) totalLiveCellCounts = calloc(numTicks, sizeof(unsigned int));

	if (DEBUG || rank == 0) printf("%d: numRanks: %d, numThreads: %d, rowsPerRank: %d, rowsPerThread: %d numTicks: %d\n",rank, numRanks,numThreads, rowsPerRank, rowsPerThread, numTicks);

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

	// allocate local heatmap chunk and initialize everything to 0
	heatmap = malloc(rowsPerRank * sizeof(int*));
    for (int i = 0; i < rowsPerRank; ++i) {
    	heatmap[i] = malloc(boardSize * sizeof(int));
    	for (int r = 0; r < boardSize; boardData[i][r++] = 0);
    }

    // init ghost rows
    if (numRanks == 1) {
    	//if we are performing a single rank run, the ghost rows just wrap around normally
    	ghostBot = boardData[0];
    	ghostTop = boardData[rowsPerRank-1];
    }
    else {
    	ghostTop = malloc(rowsPerRank * sizeof(bool*));
    	ghostBot = malloc(rowsPerRank * sizeof(bool*));
		for (int i = 0; i < boardSize; ++i) {
			ghostTop[i] = ALIVE;
			ghostBot[i] = ALIVE;
		}
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
    if (numRanks > 1) {
    	MPI_Reduce(liveCellCounts, totalLiveCellCounts, numTicks, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    }

    // timing and analysis
    double time_in_secs = (GetTimeBase() - g_start_cycles) / processor_frequency;
    if (DEBUG || rank == 0) printf("rank %d: Elapsed time = %fs\n",rank,time_in_secs);
    if (rank == 0) printf("rank %d: totalLiveCellCounts[0]=%d\n",rank, totalLiveCellCounts[0]);

	// END -Perform a barrier and then leave MPI
    MPI_Barrier( MPI_COMM_WORLD );
    MPI_Finalize();

    // cleanup dynamic memory
    free(liveCellCounts);
    free(totalLiveCellCounts);
    for (int i = 0; i < rowsPerRank; free(boardData[i++]));
    free(boardData);
	for (int i = 0; i < rowsPerRank; free(heatmap[i++]));
    free(heatmap);
    if (numRanks > 1) {
    	free(ghostTop);
    	free(ghostBot);
    }

    return 0;
}
