#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define DOWNLOAD_TAG 1
#define UPLOAD_TAG 2
#define UPDATE_TAG 3

// Structure for file information
typedef struct {
	char filename[MAX_FILENAME];
	int num_chunks;
	char chunks[MAX_CHUNKS][HASH_SIZE + 1];  // +1 for null terminator
	bool gotChunks[MAX_CHUNKS];
} FileData;

typedef struct {
	int rank;
	int numClients;
	int seeds[MAX_FILES][MAX_FILES];
	int peers[MAX_FILES][MAX_FILES];
	bool stillGoing;
} Args;

// Global variables for clients
int hasFiles;
int needsFiles;
FileData seedFor[MAX_FILES];
FileData peerFor[MAX_FILES];

void *download_thread_func(void *arg)
{
	Args *args = (Args*) arg;
	int rank = args->rank;
	int numClients = args->numClients;
	int wants = needsFiles;
	int updateTracker = 0;

	// Start downloading files
	for (int i = 0; i < wants; i++) {
		int fileID = peerFor[i].filename[4] - '0' - 1;
		int downloadedChunks = 0;
		int chunksToDownload[peerFor[i].num_chunks];
		memset(chunksToDownload, 0, sizeof(chunksToDownload));

		while (downloadedChunks < peerFor[i].num_chunks) {
			// Get a randoma chunk that has not been downloaded yet
			int currentChunk = rand() % peerFor[i].num_chunks;
			while (chunksToDownload[currentChunk]) {
				currentChunk = rand() % peerFor[i].num_chunks;
			}

			char message[HASH_SIZE + 1];

			// Try to download the chunk from a peer
			int toVisit[numClients];
			memset(toVisit, 0, sizeof(toVisit));
			toVisit[rank - 1] = 1;
			int visited = 1;
			bool found = false;

			while (visited < numClients) {
				int visit = rand() % numClients;
				while (toVisit[visit]) {
					visit = rand() % numClients;
				}
				toVisit[visit] = 1;
				visited++;

				bool check = visit >= 0 && visit < numClients && fileID >= 0 && fileID < MAX_FILES && args->peers[visit][fileID] == 1;

				// Check if the peer has the chunk
				if (check) {
					memset(message, 0, HASH_SIZE + 1);
					sprintf(message, "GET %d %d %d %d", atoi(&peerFor[i].filename[4]), currentChunk, rank, 1);    // The 1 means the message was sent to a peer
					MPI_Send(message, HASH_SIZE + 1, MPI_CHAR, visit + 1, DOWNLOAD_TAG, MPI_COMM_WORLD);

					char response[HASH_SIZE + 1];
					MPI_Recv(response, HASH_SIZE + 1, MPI_CHAR, visit + 1, UPLOAD_TAG, MPI_COMM_WORLD,
							 MPI_STATUS_IGNORE);

					if (strncmp(response, "OK", 2) == 0) {
						found = true;
						updateTracker++;

						peerFor[i].gotChunks[currentChunk] = true;
						chunksToDownload[currentChunk] = 1;
						downloadedChunks++;
						break;
					}
				}
			}

			// If the chunk was not found, try to download it from a seed
			if (!found) {
				memset(toVisit, 0, sizeof(toVisit));
				toVisit[rank - 1] = 1;
				visited = 1;

				while (visited < numClients) {
					int visit = rand() % numClients;
					while (toVisit[visit]) {
						visit = rand() % numClients;
					}
					toVisit[visit] = 1;
					visited++;

					bool check = visit >= 0 && visit < numClients && fileID >= 0 && fileID < MAX_FILES && args->seeds[visit][fileID] == 1;

					if (check) {
						memset(message, 0, HASH_SIZE + 1);
						sprintf(message, "GET %d %d %d %d", atoi(&peerFor[i].filename[4]), currentChunk, rank, 0); // The 0 means the message is sent to a seed
						MPI_Send(message, HASH_SIZE + 1, MPI_CHAR, visit + 1, DOWNLOAD_TAG, MPI_COMM_WORLD);
						char response[HASH_SIZE + 1];
						MPI_Recv(response, HASH_SIZE + 1, MPI_CHAR, visit + 1, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

						if (strncmp(response, "OK", 2) == 0) {
							updateTracker++;
							peerFor[i].gotChunks[currentChunk] = true;
							chunksToDownload[currentChunk] = 1;
							downloadedChunks++;
							break;
						}
					}
				}
			}

			// Update is needed
			if (updateTracker == 9) {
				char update[HASH_SIZE + 1];
				sprintf(update, "UPDATE %d", rank);
				// Send the update request and the rank of the peer that needs the update
				MPI_Send(update, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);
				updateTracker = 0;

				// Receive the new list of seeds and peers
				for (int i = 0; i < numClients; i++) {
					MPI_Recv(args->seeds[i], MAX_FILES, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
					MPI_Recv(args->peers[i], MAX_FILES, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
				}
			}
		}

		// Write the file to disk
		char fileName[MAX_FILENAME];
		sprintf(fileName, "client%d_file%d", rank, fileID + 1);

		FILE *f = fopen(fileName, "w");
		if (f == NULL) {
			printf("Error on opening %s\n", fileName);
			exit(-1);
		}

		for (int j = 0; j < peerFor[i].num_chunks; j++) {
			fprintf(f, "%s\n", peerFor[i].chunks[j]);
		}

		fclose(f);

		// Move the file from peerFor to seedFor
		seedFor[hasFiles] = peerFor[0];
		hasFiles++;

		needsFiles--;
		for (int j = 0; j < needsFiles; j++) {
			peerFor[i] = peerFor[i + 1];
		}

		// Tell the tracker that the peer has finished downloading the file
		char message[HASH_SIZE + 1];
		sprintf(message, "DONE %d %d", fileID + 1, rank);
		MPI_Send(message, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);
	}

	// Tell the tracker that the peer has finished downloading all the files
	char message[HASH_SIZE + 1];
	sprintf(message, "OVER");
	MPI_Send(message, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD);

	return NULL;
}

void *upload_thread_func(void *arg)
{
	char message[HASH_SIZE + 1];

	while (true) {
		memset(message, 0, HASH_SIZE + 1);

		MPI_Recv(message, HASH_SIZE + 1, MPI_CHAR, MPI_ANY_SOURCE, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		int fileID, chunkID, src, isPeer;
		sscanf(message, "GET %d %d %d %d", &fileID, &chunkID, &src, &isPeer);

		if (strncmp(message, "GET", 3) == 0) {
			bool found = false;
			char answer[HASH_SIZE + 1];

			if (isPeer) {
				for (int i = 0; i < needsFiles; i++) {
					if (peerFor[i].num_chunks <= chunkID) {
						continue;
					}

					// If the client was contacted as a peer for this file, it checks if it has the chunk
					bool check = peerFor[i].filename[4] - '0' == fileID && peerFor[i].gotChunks[chunkID];

					if (check) {
						memset(answer, 0, HASH_SIZE + 1);
						sprintf(answer, "OK %d", src);
						MPI_Send(answer, HASH_SIZE + 1, MPI_CHAR, src, UPLOAD_TAG, MPI_COMM_WORLD);
						found = true;
						break;
					}
				}

				if (!found) {
					memset(answer, 0, HASH_SIZE + 1);
					sprintf(answer, "NOK %d", src);
					MPI_Send(answer, HASH_SIZE + 1, MPI_CHAR, src, UPLOAD_TAG, MPI_COMM_WORLD);
				}
			} else {
				for (int i = 0; i < hasFiles; i++) {
					if (seedFor[i].num_chunks <= chunkID) {
						continue;
					}

					// If the client was contacted as a seed for this file, it checks if it has the file
					bool check = seedFor[i].filename[4] - '0' == fileID;

					if (check) {
						memset(answer, 0, HASH_SIZE + 1);
						sprintf(answer, "OK %d", src);
						MPI_Send(answer, HASH_SIZE + 1, MPI_CHAR, src, UPLOAD_TAG, MPI_COMM_WORLD);
						break;
					}
				}
			}
		} else if (strncmp(message, "OVER", 4) == 0) {
			break;
		}
	}

	return NULL;
}

void tracker(int numtasks, int rank) {
	FileData files[MAX_FILES];	// List of files
	int seeds[numtasks - 1][MAX_FILES];	// Files that each peer has, and it is seed for them
	int peers[numtasks - 1][MAX_FILES];	// Files that each peer needs, and becomes peer for them

	// Receive files from peers
	for (int i = 1; i < numtasks; i++) {
		int has;
		MPI_Recv(&has, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		for (int j = 0; j < has; j++) {
			char filename[MAX_FILENAME];
			MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			int fileID = filename[4] - '0' - 1;
			strncpy(files[fileID].filename, filename, MAX_FILENAME);
			MPI_Recv(&files[fileID].num_chunks, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
			MPI_Recv(files[fileID].chunks, files[fileID].num_chunks * (HASH_SIZE + 1), MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			seeds[i - 1][files[fileID].filename[4] - '0' - 1] = 1;
		}
	}

	// Sends ACK to all peers
	for (int i = 1; i < numtasks; i++) {
		MPI_Send("ACK", 4, MPI_CHAR, i, 0, MPI_COMM_WORLD);
	}

	// Receive needed files from peers
	for (int i = 1; i < numtasks; i++) {
		int num_needs;	// How many files does the peer need
		MPI_Recv(&num_needs, 1, MPI_INT, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		for (int j = 0; j < num_needs; j++) {
			char filename[MAX_FILENAME];
			MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, i, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

			// Search for the file in the list of files
			int fileID = filename[4] - '0' - 1;
			MPI_Send(&files[fileID].num_chunks, 1, MPI_INT, i, 0, MPI_COMM_WORLD);
			MPI_Send(files[fileID].chunks, files[fileID].num_chunks * (HASH_SIZE + 1), MPI_CHAR, i, 0, MPI_COMM_WORLD);

			peers[i - 1][fileID] = 1;
		}
	}

	// Send the list of seeds and peers to all peers
	for (int i = 0; i < numtasks - 1; i++) {
		for (int j = 0; j < numtasks - 1; j++) {
			MPI_Send(seeds[i], MAX_FILES, MPI_INT, j + 1, 0, MPI_COMM_WORLD);
			MPI_Send(peers[i], MAX_FILES, MPI_INT, j + 1, 0, MPI_COMM_WORLD);
		}
	}

	// Answer to the peers' requests for updates and other stuff
	int completed = 0;
	while (completed < numtasks - 1) {
		char update[HASH_SIZE + 1];
		MPI_Recv(update, HASH_SIZE + 1, MPI_CHAR, MPI_ANY_SOURCE, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

		if (strncmp(update, "UPDATE", 6) == 0) {
			int peerRank;
			sscanf(update, "UPDATE %d", &peerRank);

			// Send the list of seeds and peers to the peers requesting the update
			for (int i = 0; i < numtasks - 1; i++) {
				MPI_Send(seeds[i], MAX_FILES, MPI_INT, peerRank, UPDATE_TAG, MPI_COMM_WORLD);
				MPI_Send(peers[i], MAX_FILES, MPI_INT, peerRank, UPDATE_TAG, MPI_COMM_WORLD);
			}
		}

		// Update the list of seeds and peers
		if (strncmp(update, "DONE", 4) == 0) {
			int fileID, peerRank;
			sscanf(update, "DONE %d %d", &fileID, &peerRank);

			peers[peerRank - 1][fileID - 1] = 0;
			seeds[peerRank - 1][fileID - 1] = 1;
		}

		// If there are no more peers, stop the tracker
		if (strncmp(update, "OVER", 4) == 0) {
			completed++;
		}
	}

	// If all the clients have finished downloading, tell their upload thread to stop
	for (int i = 1; i < numtasks; i++) {
		char update[HASH_SIZE + 1];
		sprintf(update, "OVER");
		MPI_Send(update, HASH_SIZE + 1, MPI_CHAR, i, DOWNLOAD_TAG, MPI_COMM_WORLD);
	}
}

void peer(int numtasks, int rank) {
	pthread_t download_thread;
	pthread_t upload_thread;
	void *status;
	int r;

	// Which file to read
	char name[MAX_FILENAME];
	sprintf(name, "in%d.txt", rank);

	FILE *f = fopen(name, "r");
	if (f == NULL) {
		printf("Error on opening %s\n", name);
		exit(-1);
	}

	// Read the files I possess
	fscanf(f, "%d", &hasFiles);
	MPI_Send(&hasFiles, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

	// Tell tha tracker what files does it have, and send the hashes of the chunks
	for (int i = 0; i < hasFiles; i++) {
		fscanf(f, "%s %d\n", seedFor[i].filename, &seedFor[i].num_chunks);
		MPI_Send(seedFor[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
		MPI_Send(&seedFor[i].num_chunks, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

		for (int j = 0; j < seedFor[i].num_chunks; j++) {
			fscanf(f, "%s\n", seedFor[i].chunks[j]);
		}

		MPI_Send(seedFor[i].chunks, seedFor[i].num_chunks * (HASH_SIZE + 1), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
	}

	// Read the files I need
	fscanf(f, "%d", &needsFiles);

	for (int i = 0; i < needsFiles; i++) {
		fscanf(f, "%s\n", peerFor[i].filename);
		peerFor[i].num_chunks = -1;
		memset(peerFor[i].gotChunks, 0, sizeof(peerFor[i].gotChunks));
	}

	fclose(f);	// Close file

	// Wait for ACK
	char ack[4];
	MPI_Recv(ack, 4, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

	// get from the tracker the lists with the hashes of the files I want
	MPI_Send(&needsFiles, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
	for (int i = 0; i < needsFiles; i++) {
		MPI_Send(peerFor[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);	// Ask for a certain file
		MPI_Recv(&peerFor[i].num_chunks, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	// How many chunks does the file have
		MPI_Recv(peerFor[i].chunks, peerFor[i].num_chunks * (HASH_SIZE + 1), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);	// The hashes of the chunks
	}

	// Receive the list of seeds and peers
	int seeds[numtasks - 1][MAX_FILES];
	int peers[numtasks - 1][MAX_FILES];

	for (int i = 0; i < numtasks - 1; i++) {
		MPI_Recv(seeds[i], MAX_FILES, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
		MPI_Recv(peers[i], MAX_FILES, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
	}

	Args* args = (Args*) malloc(sizeof(Args));
	args->rank = rank;
	args->numClients = numtasks - 1;
	memcpy(args->seeds, seeds, sizeof(seeds));
	memcpy(args->peers, peers, sizeof(peers));
	args->stillGoing = true;

	r = pthread_create(&download_thread, NULL, download_thread_func, (void *) args);
	if (r) {
		printf("Eroare la crearea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) args);
	if (r) {
		printf("Eroare la crearea thread-ului de upload\n");
		exit(-1);
	}

	r = pthread_join(download_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_join(upload_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de upload\n");
		exit(-1);
	}

	free(args);
}

int main (int argc, char *argv[]) {
	int numtasks, rank;

	int provided;
	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	if (provided < MPI_THREAD_MULTIPLE) {
		fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
		exit(-1);
	}
	MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	if (rank == TRACKER_RANK) {
		tracker(numtasks, rank);
	} else {
		peer(numtasks, rank);
	}

	MPI_Finalize();
}
