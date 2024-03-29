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
#define BOARDTESTING false

#define ALIVE 1
#define DEAD  0

// output parameters (for performance testing)
#define OUTPUTHEATMAP true
#define OUTPUTBOARD false

// MPI data
int numRanks = -1; // total number of ranks in the current run
int rank = -1; // our rank
// game/board info
int threshold; // probability that cells will randomize
#define boardSize 32768 // total # of rows on a square grid
int numThreads = -1; // total number of threads at each rank (including the rank itself, meaning we spawn numThreads-1 pthreads); passed in as arg1
pthread_barrier_t threadBarrier;
int numTicks = -1; // how many ticks of the simulation to perform; passed in as arg2
bool** boardData;
int** heatmap;
int* heatmapAll;
bool* ghostTop;
bool* ghostBot;
int rowsPerRank = -1; // how many rows each rank is responsible for
int rowsPerThread = -1; // how many rows each thread is responsible for
int globalIndex; // this row's global index
// timing/stats
double g_time_in_secs = 0;
unsigned long long g_start_cycles=0;
unsigned int* liveCellCounts; // array of ints corresponding to the # of live cells at the end of each simulation tick
unsigned int* totalLiveCellCounts; // live cell counts across all ranks combined
pthread_mutex_t counterMutex = PTHREAD_MUTEX_INITIALIZER; // lock used to ensure safe thread counting

/**
 * return the number of live neighbors of the cell at the specified row/col, wrapping around at the edges of the board
 * @param row: the row at which the desired cell resides
 * @param col: the column at which the desired cell resides
 * @return: the live neighbor count at the specified cell
 */
int countNeighbors(int row, int col) {
	// by storing pointers to the top and bottom rows and the left and right column positions up-front, this routine should be very efficient
	bool* botRow = (row == rowsPerRank-1) ? ghostBot : boardData[row+1];
	bool* topRow = (row == 0) ? ghostTop : boardData[row-1];
	int leftCol = (col == 0) ? boardSize-1 : col-1;
	int rightCol = (col == boardSize-1) ? 0 : col+1;
	//     topLeft         + topCenter   + topRight         + centerLeft              + centerRight              + bottomLeft      + bottomCenter+ bottomRight
	return topRow[leftCol] + topRow[col] + topRow[rightCol] + boardData[row][leftCol] + boardData[row][rightCol] + botRow[leftCol] + botRow[col] + botRow[rightCol];
}

/**
 * Copies the current tick data into the cumulative heatmap
 */
void updateHeatmap() { 
	for (int i = 0; i < rowsPerRank; ++i){
		for (int j = 0; j < boardSize; ++j){
			heatmap[i][j] += boardData[i][j];  // use me for heatmap across all ticks
			//heatmap[i][j] = boardData[i][j];  // use me for heatmap only at final state
		}
	}
}

/**
 * Write the board state to a file
 */
void outputBoard() {
	MPI_File boardFile;
	printf("rank %d starting outputBoard\n", rank);
	MPI_File_open(MPI_COMM_WORLD, "final_board.out", MPI_MODE_CREATE | MPI_MODE_WRONLY, MPI_INFO_NULL, &boardFile);
	printf("rank %d opened file\n", rank);
	char* writebuffer = malloc(sizeof(char) * (boardSize+1));
	printf("rank %d allocated writebuffer\n", rank);
	writebuffer[boardSize] = '\n';
	for (int k = 0; k<rowsPerRank; ++k) {
		for (int r = 0; r < boardSize; ++r) {
			writebuffer[r] = boardData[k][r]?'x':'_';
		}
		MPI_File_write_at(boardFile, (rank*rowsPerRank+k)*(boardSize+1), writebuffer, boardSize+1, MPI_CHAR, MPI_STATUS_IGNORE);
	}
	printf("rank %d finished writing\n", rank);
	free(writebuffer);
	printf("rank %d freed buffer\n", rank);
	MPI_File_close(&boardFile);
	printf("rank %d closed board file\n", rank);
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

/**
 * write the heatmap to a file, ensuring order by sharing heatmap chunks over MPI
 */
void outputHeatmap(){
	// non-0 ranks simply share their heatmap chunk with rank 0
	printf("rank %d starting outputHeatmap\n", rank);
	if(rank != 0){
		printf("rank %d sending...\n", rank);
		MPI_Send(heatmapAll, boardSize*rowsPerRank, MPI_INT, 0, 0, MPI_COMM_WORLD);
		printf("rank %d sent\n", rank);
		return;
	}

	// rank 0 writes his chunk to the heatmap output file
	FILE* heatmapFile =	fopen(threshold == 0 ? "hm0.out" : (threshold == 25 ? "hm25.out" : (threshold == 50 ? "hm50.out" : "hm75.out")), "w");
	printf("opened heatmap file\n");
	for(int y = 0; y < rowsPerRank; y+=32){
		for(int x = 0; x < boardSize; x+=32){
			int groupSum = 0;
			for (int y2 = y; y2 < y+32; ++y2) {
				for (int x2 = x; x2 < x+32; ++x2) {
					groupSum += heatmapAll[y2*boardSize+x2];
				}
			}
			fprintf(heatmapFile, "%d ", groupSum);
			if (DEBUG) printf("%d ", groupSum);
		}
		fprintf(heatmapFile, "\n");
		if (DEBUG) printf("\n");
	}

	// finally, rank 0 receives heatmap chunks from the remaining ranks in order, and outputs those chunks to the heatmap output file
	int* heatmapRecv = malloc(sizeof(int) * boardSize * rowsPerRank);
	for (int i = 1; i<numRanks; ++i) {
		printf("receiving from rank %d...\n", i);
		MPI_Recv(heatmapRecv, boardSize*rowsPerRank, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		printf("received from rank %d\n", i);
		for(int y = 0; y < rowsPerRank; y+=32){
			for(int x = 0; x < boardSize; x+=32){
				int groupSum = 0;
				for (int y2 = y; y2 < y+32; ++y2) {
					for (int x2 = x; x2 < x+32; ++x2) {
						groupSum += heatmapRecv[y2*boardSize+x2];
					}
				}
				fprintf(heatmapFile, "%d ", groupSum);
			}
			fprintf(heatmapFile, "\n");
		}
		printf("printed data from rank %d\n", i);
	}
	free(heatmapRecv);
	fclose(heatmapFile);

	fflush(stdout);
}

/**
 * run the simulation for numTicks steps. Called by each thread on each rank.
 * @param threadNum: an int containing the calling thread's index on its local rank
 */

void *runSimulation(void* threadNum) {
	int threadId = *((int*)threadNum);
	printf("rank %d thread %d starting\n", rank, threadId);
	MPI_Request sReqTop, sReqBot, rReqTop, rReqBot;
	MPI_Status status;
	if (BOARDTESTING && threadId == 0) printBoard();
	for (int i = 0; i < numTicks; ++i) {
		// exchange row data with top and bottom neighbors
		if (numRanks > 1 && threadId == 0) {
			// send top
			MPI_Isend(boardData[rowsPerRank-1], boardSize, MPI_C_BOOL, rank==numRanks-1?0:rank+1, 0, MPI_COMM_WORLD, &sReqTop);
			// send bottom
			MPI_Isend(boardData[0], boardSize, MPI_C_BOOL, rank==0?numRanks-1:rank-1, 0, MPI_COMM_WORLD, &sReqBot);
			// receive top
			MPI_Irecv(ghostBot, boardSize, MPI_C_BOOL, rank==numRanks-1?0:rank+1, 0, MPI_COMM_WORLD, &rReqTop);
			// receive bottom
			MPI_Irecv(ghostTop, boardSize, MPI_C_BOOL, rank==0?numRanks-1:rank-1, 0, MPI_COMM_WORLD, &rReqBot);
			// make sure we've received the ghost data before proceeding
			MPI_Wait(&rReqTop, &status);
			MPI_Wait(&rReqBot, &status);
		}
		// sync threads on the current tick after we've received ghost rows
		pthread_barrier_wait(&threadBarrier);
		// update grid
		unsigned int localLiveCells = 0;
		for (int k = rowsPerThread*threadId; k < rowsPerThread*(threadId+1); ++k) {
			for (int r = 0; r < boardSize; ++r) {
				// consult rng and count neighbors to determine cell state
				double random = GenVal(k+globalIndex)*100;  // shift rng range from 0-1 to 0-100
				// when we don't reach the random threshold, treat >= threshold/2 as false, and < threshold/2 as true
				if (random < threshold) boardData[k][r] = ( random < (threshold>>1) );
				else {
					int neighbors = countNeighbors(k,r);
					boardData[k][r] = neighbors == 3 || (neighbors == 2 && boardData[k][r] == ALIVE);
				}
				// keep track of local live cells
				localLiveCells += boardData[k][r];
			}
		}
		// grab the counter lock for the increment critical section
		while (pthread_mutex_trylock(&counterMutex) != 0);
		liveCellCounts[i]+=localLiveCells;
		pthread_mutex_unlock(&counterMutex);
		if (rank == 0 && threadId == 0) printf("%d\n",i);
		if (BOARDTESTING && threadId == 0) printBoard();
		if (OUTPUTHEATMAP) updateHeatmap();

	}
	printf("rank %d thread %d finished\n", rank, threadId);
	// all done with thread barriers
	return NULL;
}

int main(int argc, char *argv[]) {
	// usage check
	if (argc != 4) {
		fprintf(stderr,"Error: %d input argument[s] were supplied, but 3 were expected. Usage: mpirun -np X %s numThreadsPerRank numTicks threshold\n",argc-1, argv[0]);
		exit(1);
	}
	// grab run parameters from cmd args
	numThreads = atoi(argv[1]);
	numTicks = atoi(argv[2]);
	threshold = atof(argv[3]);

	// use a pthread barrier to ensure threads don't tick on stale data
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
	// heatmap is allocated contiguously so it can be sent over mpi more easily at the end
	if (OUTPUTHEATMAP) {
		heatmap = malloc(rowsPerRank * sizeof(int*));
		heatmapAll = malloc(rowsPerRank * boardSize * sizeof(int*));
		for (int i = 0; i < rowsPerRank; ++i) {
			heatmap[i] = &(heatmapAll[boardSize * i]);
			for (int r = 0; r < boardSize; heatmap[i][r++] = 0);
		}
	}

    // init ghost rows
    if (numRanks == 1) {
    	// if we are performing a single rank run, the ghost rows just wrap around normally
    	ghostBot = boardData[0];
    	ghostTop = boardData[rowsPerRank-1];
    }
    else {
    	// if we are performing a multi-rank run, we need proper ghost rows of size boardSize
    	ghostTop = malloc(boardSize * sizeof(bool));
    	ghostBot = malloc(boardSize * sizeof(bool));
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

	printf("rank %d joined threads\n", rank);
    pthread_barrier_destroy(&threadBarrier);
	printf("rank %d destroyed barrier\n", rank);
    // sum the live cell counts across all ranks
    if (numRanks > 1) {
    	MPI_Reduce(liveCellCounts, totalLiveCellCounts, numTicks, MPI_UNSIGNED, MPI_SUM, 0, MPI_COMM_WORLD);
    } else {
    	// copy over the local live cell counts if we're in a single rank run
    	for (int i = 0; i < numTicks; totalLiveCellCounts[i] = liveCellCounts[i],++i);
    }
	printf("rank %d finished sync\n", rank);

    // timing and analysis
    double time_in_secs = (GetTimeBase() - g_start_cycles) / processor_frequency;
    if (DEBUG || rank == 0) printf("rank %d: Elapsed time = %fs\n",rank,time_in_secs);
    if (rank == 0) {
    	printf("liveCellCounts =");
    	for (int i = 0; i < numTicks; ++i) {
    		printf(" %d:%d",i, totalLiveCellCounts[i]);
    	}
    	printf("\n");
    }

	// output data to files
	if (OUTPUTBOARD) {
		g_start_cycles = GetTimeBase();
		outputBoard();
		time_in_secs = (GetTimeBase() - g_start_cycles) / processor_frequency;
		if (DEBUG || rank == 0) printf("rank %d: Board Write Elapsed time = %fs\n",rank,time_in_secs);
	}
	if (OUTPUTHEATMAP) outputHeatmap();
	
	// END -Perform a barrier and then leave MPI
    MPI_Barrier( MPI_COMM_WORLD );
    MPI_Finalize();

    // cleanup dynamic memory
    free(liveCellCounts);
    free(totalLiveCellCounts);
    for (int i = 0; i < rowsPerRank; free(boardData[i++]));
    free(boardData);
    // nothing to free if we're not using a heatmap for the current simulation
    if (OUTPUTHEATMAP) {
		free(heatmap);
		free(heatmapAll);
    }
    // nothing to free if we're only using a single rank for the current simulation
    if (numRanks > 1) {
    	free(ghostTop);
    	free(ghostBot);
    }
    return 0;
}
