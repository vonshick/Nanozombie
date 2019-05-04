#include <mpi.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <queue>
#include <unistd.h>

#define BOATS_STATUS_MSG 0
#define WANNA_PONNY_MSG 1

using namespace std;

const int VISITOR_MAX_WAIT = 10;    //seconds
const int VISITOR_MIN_WAIT = 2;     //seconds

struct ListeningThreadData
{
    bool *run;
    queue<int> *ponyQueue;
    queue<int> *boatQueue;
    MPI_Status *status;
    //todo: mutexes for above
};

struct TransmittingThreadData
{
    bool *run;
};

struct Boat
{
    int msg_type;
    int capacity;       
    bool onTrip;   //true - on trip, false - boat in port
    int captainId; //process being captain on current trip
};

//function for listening thread - receiving messages
void *listen(void *voidThreadData)
{
    pthread_detach(pthread_self());
    ListeningThreadData *threadData = (ListeningThreadData *) voidThreadData;
    Boat *buffer = new Boat;   
    MPI_Status *status = threadData->status;
    while(*(threadData->run))
    {
        MPI_Recv(buffer, sizeof(Boat), MPI_BYTE, 0, 0, MPI_COMM_WORLD, status); 
        switch(buffer->msg_type){
            case WANNA_PONNY_MSG:
                printf("WANNA_PONNY_MSG");
                break;
            default:
                break; 
        }
        //todo
        //MPI_Recv(&sorted, TABSIZE-(size-1), MPI_INT, size-1, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    }
    delete buffer;
    pthread_exit(NULL);        
}

//function for transmitting thread - sending messages
void *transmit(void *voidThreadData)
{
    pthread_detach(pthread_self());
    TransmittingThreadData *threadData = (TransmittingThreadData *) voidThreadData;
    while(*(threadData->run))
    {
        //todo
        //pthread_cond_wait, pthread_cond_signal - wait for something to send
        //MPI_Send(&max, 1, MPI_INT, rank+1, END, MPI_COMM_WORLD);  
    }  
    pthread_exit(NULL);        
}

//main thread function, visitor logic
void visit(bool *run, Boat *boats, queue<int> *ponyQueue, queue<int> *boatQueue, int *rank, int *size, MPI_Status *status)
{
    while(*run)
    {

        int wait_milisec = (rand() % (VISITOR_MAX_WAIT-VISITOR_MIN_WAIT)*1000) + VISITOR_MIN_WAIT*1000;
        usleep(wait_milisec*1000);

        Boat *wanna_ponny = new Boat;
        (*wanna_ponny).msg_type = WANNA_PONNY_MSG;
        for(int i = 1; i < (*size); i++)
        {
            MPI_Send( wanna_ponny, sizeof(Boat), MPI_BYTE, i, 0, MPI_COMM_WORLD); //send array with boats capacity
        }    


        //now he wants a trip!
        //todo
    }
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Status status;
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if(argc != 5)
    {
	    MPI_Finalize();
	    cout << "Specify 4 arguments: numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorWeight";
	    exit(0);
    }

    int numberOfPonies = atoi(argv[1]);
    const int numberOfBoats = atoi(argv[2]);
    const int maxBoatCapacity = atoi(argv[3]);
    const int maxVisitorWeight = atoi(argv[4]);
    //initialization
    //data, variables
    srand(time(NULL));   
    bool run = true;
    Boat *boats = new Boat[numberOfBoats]; //capacity of each boat 
    if (rank == 0)
    {
        for(int i = 0; i < numberOfBoats; i++)
        {
            boats[i].msg_type = BOATS_STATUS_MSG;
            boats[i].capacity = (rand() % maxBoatCapacity)  + 1;
            boats[i].onTrip = false;
        }
        for(int i = 1; i < size; i++)
        {
            MPI_Send(boats, numberOfBoats * sizeof(Boat), MPI_BYTE, i, 0, MPI_COMM_WORLD); //send array with boats capacity
        }        
    }
    else
    {
        MPI_Recv(boats, numberOfBoats * sizeof(Boat), MPI_BYTE, 0, 0, MPI_COMM_WORLD, &status); //wait for boats capacity
    }

    queue<int> *ponyQueue;  //queue for pony, storing process id
    queue<int> *boatQueue;  //queue for boat

    //create listening thread - receiving messages
    ListeningThreadData *listeningThreadData = new ListeningThreadData;
    listeningThreadData->run = &run;
    listeningThreadData->status = &status;
    listeningThreadData->ponyQueue = ponyQueue;
    listeningThreadData->boatQueue = boatQueue;
    pthread_t listeningThread;
    if (pthread_create(&listeningThread, NULL, listen, (void *) listeningThreadData))
    {
        cout << "Error encountered creating thread.";
        MPI_Finalize();
        exit(0);
    }

    //create transmitting thread - receiving messages
    TransmittingThreadData *transmittingThreadData = new TransmittingThreadData;
    transmittingThreadData->run = &run;
    pthread_t transmittingThread;
    if (pthread_create(&transmittingThread, NULL, transmit, (void *) transmittingThreadData))
    {
        cout << "Error encountered creating thread.";
        MPI_Finalize();
        exit(0);
    }

//initializatin completed, starting main logic
    visit(&run, boats, ponyQueue, boatQueue, &rank, &size, &status);

    MPI_Finalize();
    delete[] boats;
    delete listeningThreadData;
    delete transmittingThreadData;
}
