#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_PEERS 10
#define min(a, b) ((a) < (b) ? (a) : (b))

typedef struct {
    char filename[MAX_FILENAME]; //nume
    int chunks_count; //nr de segmente
    int client_count[MAX_CHUNKS]; //nr de clienti care au segmentul
    int clients[MAX_CHUNKS][MAX_PEERS]; //clientii care au segmentul
} FileInfoTracker;

typedef struct {
    char filename[MAX_FILENAME]; //nume
    int chunks_count; //nr de segmente
    char **segments; //segmentele
} FileOwn;


FileOwn *files_own;
FileOwn *files_to_download;
int nr_files;
int nr_files_own;
int download_remaning[MAX_FILES]; //la ce segment a ramas de dowlandat pt fiecare file
int nr_files_to_download;

void *download_thread_func(void *arg)
{
    int rank = *(int*) arg;
    for(int i = 0; i < nr_files_to_download; i++) { //pentru fiecare file de downloadat
        // trimit trackerului numele file ului pe care mi l doresc
        MPI_Send(files_to_download[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
        FileInfoTracker *file_info = calloc(sizeof(FileInfoTracker), 1);
        //primesc datele despre el: cate seg sunt, nr de clienti pt fiecare seg si cine il detine
        MPI_Recv(&file_info->chunks_count, 1, MPI_INT, TRACKER_RANK, 8, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(file_info->client_count, file_info->chunks_count, MPI_INT, TRACKER_RANK, 8, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(file_info->clients, file_info->chunks_count * MAX_PEERS, MPI_INT, TRACKER_RANK, 8, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        while(download_remaning[i] < file_info->chunks_count) {
            
            // alege un peer random din lista de peeruri pt seg la care a ramas de downloadat
            int rand = (random() % file_info->client_count[download_remaning[i]]);
            int peer = file_info->clients[download_remaning[i]][rand];
            // trimite peerului ales numele file ului pe care il vrea
            MPI_Send(files_to_download[i].filename, MAX_FILENAME, MPI_CHAR, peer, 2, MPI_COMM_WORLD);
            int *how_many = malloc(sizeof(int));
            // clientul poate vrea 10, sau daca se afla la finalul file ului, poate dori mai putine
            *how_many = min(10, file_info->chunks_count - download_remaning[i]);
            // trimite peerului nr de seg pe care si l doreste
            MPI_Send(how_many, 1, MPI_INT, peer, 15, MPI_COMM_WORLD);
            for(int j = download_remaning[i]; j < download_remaning[i] + *how_many; j++) {
                int *seg = malloc(sizeof(int));
                *seg = j;
                // trimite nr seg dorit
                MPI_Send(seg, 1, MPI_INT, peer, 9, MPI_COMM_WORLD);
                // primeste segmentul
                MPI_Recv(files_to_download[i].segments[*seg], HASH_SIZE, MPI_CHAR, peer, 5, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                int ok = 0;
                // daca nu e primul segment pentru file ul resp, deci el exista in files_own
                // actualizeaza segmentele de acolo
                for(int k = 0; k < nr_files_own; k++) {
                    if(strcmp(files_own[k].filename, files_to_download[i].filename) == 0) {
                        ok = 1;
                        strcpy(files_own[k].segments[*seg], files_to_download[i].segments[*seg]);
                        break;
                    }
                }
                // daca e primul seg, trebuie sa adauge noul file ca own si se pune si segmentul
                if(!ok) {
                    strcpy(files_own[nr_files_own].filename, files_to_download[i].filename);
                    strcpy(files_own[nr_files_own].segments[*seg], files_to_download[i].segments[*seg]);
                    nr_files_own++;
                }
            }
            //actualizez trackerul
            char *start_act = malloc(MAX_FILENAME);
            strcpy(start_act, "start"); //mesaj pt tracker sa stie ca primeste o actualizare pt swarm
            MPI_Send(start_act, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
            MPI_Send(files_to_download[i].filename, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
            MPI_Send(how_many, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
            for(int j = download_remaning[i]; j < download_remaning[i] + *how_many; j++) {
                MPI_Send(&j, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD);
            }
            // trece la urm segemnte
            download_remaning[i] += *how_many;
        }
        // scrie in fisier dupa ce a terminat downloadarea completa a file ului 
        char newfilename[50];
        snprintf(newfilename, sizeof(newfilename), "client%d_%s", rank, files_to_download[i].filename);
        FILE *file = fopen(newfilename, "w");
        for(int j = 0; j < file_info->chunks_count; j++) {
            fprintf(file, "%s\n", files_to_download[i].segments[j]);
        }
        fclose(file);
    }
    // dupa ce a terminat tot ce avea de downloadata, trimite trackerului mesaj de finish
    char *q = malloc(5);
    strcpy(q, "stop");
    MPI_Send(q, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
    return NULL;
}

void *upload_thread_func(void *arg)
{
    int rank = *(int*) arg;
    MPI_Status status;
    while(1) {
        char *filename = malloc(MAX_FILENAME);
        // primeste ce file trebuie sa dea
        MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, 2, MPI_COMM_WORLD, &status);
        // daca e mesaj de la tracker ca trebuie sa se opreasca
        if(strcmp(filename, "stop") == 0) {
            break;
        }
        // primeste cate trebuie sa dea
        int *how_many = malloc(sizeof(int));
        MPI_Recv(how_many, 1, MPI_INT, status.MPI_SOURCE, 15, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        int i;
        // cauta file ul
        for (i = 0; i < nr_files_own; i++) {
            if (strcmp(files_own[i].filename, filename) == 0) {
                break;
            }
        }
        // primeste nr seg dorit si il trimite
        for(int j = 0; j < *how_many; j++) {
            int *seg = malloc(sizeof(int));
            MPI_Recv(seg, 1, MPI_INT, status.MPI_SOURCE, 9, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Send(files_own[i].segments[*seg], HASH_SIZE, MPI_CHAR, status.MPI_SOURCE, 5, MPI_COMM_WORLD);
            
        }
    }

    return NULL;
}

void tracker(int numtasks, int rank) {
    MPI_Status status;
    int clients_satisfied = 0;
    int clients_dones = 0;
    int nr_files_recv;
    FileInfoTracker *files;
    files = calloc(sizeof(FileInfoTracker), MAX_FILES);
    // cat timp n au trimis toti datele
    while (clients_dones < numtasks - 1) {
       // primeste cate file uri o sa puna pt respectivul peer
        MPI_Recv(&nr_files_recv, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        for (int i = 0; i < nr_files_recv; i++) {
            FileInfoTracker *file_info = calloc(sizeof(FileInfoTracker), 1);
            MPI_Recv(file_info, sizeof(FileInfoTracker), MPI_BYTE, status.MPI_SOURCE, 0, MPI_COMM_WORLD, &status);
            int ok = 0;
            for (int j = 0; j < nr_files; j++) {
                // daca avea deja file ul de la alt peer, il adauga pe cel curent in lista de clienti pt fiecare seg
                if (strcmp(files[j].filename, file_info->filename) == 0) {
                    ok = 1;
                    for (int k = 0; k < file_info->chunks_count; k++) {
                        files[j].client_count[k] += 1;
                        files[j].clients[k][files[j].client_count[k] - 1] = status.MPI_SOURCE;
                    }
                    break;
                }
            }
            // daca nu, adauga file ul
            if (!ok) {
                files[nr_files] = *file_info;
                nr_files++;
            }
        }

        clients_dones++;
    }
    // dupa ce a terminat de colectat datele de la toti, trimite un ack ca pot incepe downloadul
    for (int i = 1; i < numtasks; i++) {
        MPI_Send("ACK", 3, MPI_CHAR, i, 0, MPI_COMM_WORLD);
    }
    // cat timp au de downloadat clientii
    while(clients_satisfied < numtasks-1) {
        char *filename = malloc(MAX_FILENAME);
        MPI_Recv(filename, MAX_FILENAME, MPI_CHAR, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
        // daca a primit un finish de download
        if (strcmp(filename, "stop") == 0) {
            clients_satisfied++;
            continue;
        }
        // daca a primit info de actualizat 
        else if(strcmp(filename, "start") == 0) {
            char *filename2 = malloc(MAX_FILENAME);
            MPI_Recv(filename2, MAX_FILENAME, MPI_CHAR, status.MPI_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            int *how_many = malloc(sizeof(int));
            MPI_Recv(how_many, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            for( int i = 0; i < *how_many; i++)
            {
                int *seg = malloc(sizeof(int)); 
                MPI_Recv(seg, 1, MPI_INT, status.MPI_SOURCE, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                for(int j = 0; j < nr_files; j++) {
                    // gaseste file ul si pune peerul ca owner pe segmentele resp
                    if(strcmp(files[j].filename, filename2) == 0) {
                        files[j].clients[*seg][files[j].client_count[*seg]] = status.MPI_SOURCE;
                        files[j].client_count[*seg] += 1;
                        break;
                    }
                }
                free(seg);
            }
            free(filename2);
            free(how_many);
            continue;
        }
        // daca nu se incadreaza in conditiile de mai sus, trebuie sa trimita info despre un file
        for (int i = 0; i < nr_files; i++) {
            if (strcmp(files[i].filename, filename) == 0) {
                MPI_Send(&files[i].chunks_count, 1, MPI_INT, status.MPI_SOURCE, 8, MPI_COMM_WORLD);
                MPI_Send(files[i].client_count, files[i].chunks_count, MPI_INT, status.MPI_SOURCE, 8, MPI_COMM_WORLD);
                MPI_Send(files[i].clients, files[i].chunks_count * MAX_PEERS, MPI_INT, status.MPI_SOURCE, 8, MPI_COMM_WORLD);
                break;
            }
        }
        free(filename);
    }
    
    char *message = malloc(MAX_FILENAME);
    // trimite stop cand au terminat toti clientii pentru a inchide threadurile de upload
    strcpy(message, "stop");
    for (int i = 1; i < numtasks; i++) {
        printf("Tracker sent -1 to peer %d\n", i);
        MPI_Send(message, MAX_FILENAME, MPI_CHAR, i, 2, MPI_COMM_WORLD);}
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r=0;
    FILE *file;
    char filename[50];
    char line[100];
    snprintf(filename, sizeof(filename), "in%d.txt", rank);
    file = fopen(filename, "r");
    fgets(line, sizeof(line), file);
    int nr_files = atoi(line);
    // trimite cate fileuri va trimite
    MPI_Send(&nr_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    for (int i = 0; i < nr_files; i++) {
        fgets(line, sizeof(line), file);
        char *pch = strtok(line, " ");
        char *file_in = pch;
        pch = strtok(NULL, " ");
        int chunks_count = atoi(pch);
        FileInfoTracker *file_info = calloc(sizeof(FileInfoTracker), 1);
        files_own[nr_files_own].chunks_count = chunks_count;
        strcpy(file_info->filename, file_in);
        strcpy(files_own[nr_files_own].filename, file_in);
        file_info->chunks_count = chunks_count;
        for (int j = 0; j < chunks_count; j++) {
            // are un singur client la inceput, el insusi
            file_info->client_count[j] = 1;
            fgets(line, sizeof(line), file);
            char *hash = malloc(HASH_SIZE);
            hash = strtok(line, " \n");
            file_info->clients[j][0] = rank;
            // pune hash urile in files_own
            strcpy(files_own[nr_files_own].segments[j], hash);

        }
        nr_files_own++;
        // trimite info despre file la tracker
        MPI_Send(file_info, sizeof(FileInfoTracker), MPI_BYTE, TRACKER_RANK, 0, MPI_COMM_WORLD);
        
    }
    char ack[3];
    MPI_Recv(ack, 3, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
   
    fgets(line, sizeof(line), file);
    nr_files_to_download = atoi(line);
    r = -1;
    for(int i = 0; i < nr_files_to_download; i++) {
        fgets(line, sizeof(line), file);
        char *pch = strtok(line, " \n");
        pch[strlen(pch)] = '\0';
        // pune numele file urilor de downloadat in files_to_download
        strcpy(files_to_download[i].filename, pch);
        download_remaning[i] = 0;
    }
    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }
    fclose(file);
    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
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

    // alocare spatiu pt files_own & files_to_download
    files_own = malloc(sizeof(FileOwn)*MAX_FILES);
    for (int i = 0; i < MAX_FILES; i++) {
        files_own[i].segments = malloc(HASH_SIZE * MAX_CHUNKS);
        for (int j = 0; j < MAX_CHUNKS; j++) {
            files_own[i].segments[j] = malloc(HASH_SIZE);
        }
    }
    files_to_download = malloc(sizeof(FileOwn)*MAX_FILES);
    for (int i = 0; i < MAX_FILES; i++) {
        files_to_download[i].segments = malloc(HASH_SIZE * MAX_CHUNKS);
        for (int j = 0; j < MAX_CHUNKS; j++) {
            files_to_download[i].segments[j] = malloc(HASH_SIZE);
        }
    }

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
