#ifndef HEADER
#define HEADER

#define MAX_LINE_LENGTH 4096
#define BUFFER_SIZE 4096
#define CHUNK_SIZE 4096
#define SERVER_PORT 6880
#define SERVER_IP "127.0.0.1"
#define PEER_PORT 6883
#define MY_PEER_PORT 6889
#define TIMEOUT_SEC 5  
#define NUM_THREADS 8


struct open_files
{
    char* file_name;
    int num_chunks;
    int *chunks;
    struct open_files* next;
};

struct send_peer{
    int peer_sock;
    char *file_name;
    int start_chunk;
    int num_chunk;
};

int file_is_receiving(char* filename);
int delete_file_from_list(char *filename);

#endif