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

int numberOfPonies;
int numberOfBoats;
int maxBoatCapacity;
int maxVisitorWeight;

pthread_cond_t ponySuitCond;
pthread_mutex_t ponySuitMutex;

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

struct Packet
{
    int msgType; 
    int id; //id of tourist
    int capacity;       
    bool boatOnTrip;   //true - on trip, false - boat in port
    int captainId; //process being captain on current trip
};

//function for listening thread - receiving messages
void *listen(void *voidThreadData)
{

    pthread_detach(pthread_self());
    ListeningThreadData *threadData = (ListeningThreadData *) voidThreadData;
    Packet *buffer = new Packet;
    MPI_Status *status = threadData->status;
    
    while(*(threadData->run))
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, status); 
        //check what kind of message came and call proper event
        switch(buffer->msgType){
            case WANNA_PONNY_MSG:
                cout<<"Tourist "<< buffer->id <<" want a pony suit!\n";
                pthread_cond_signal(&ponySuitCond); //that's only for now - to test pthread_cond_signal
                //todo
                //send message about having pony suit or not
                break;
            default:
                break; 
        }
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
void visit(bool *run, Packet *boats, queue<int> *ponyQueue, queue<int> *boatQueue, int *rank, int *size, MPI_Status *status)
{   

    while(*run)
    {
        cout<<"Tourist "<<(*rank)<<" is registered!\n";

        int wait_milisec = (rand() % (VISITOR_MAX_WAIT-VISITOR_MIN_WAIT)*1000) + VISITOR_MIN_WAIT*1000;
        usleep(wait_milisec*1000);

        Packet *wanna_ponny = new Packet;
        (*wanna_ponny).msgType = WANNA_PONNY_MSG;
        (*wanna_ponny).id = (*rank);

        for(int i = 0; i < (*size); i++)
        {
            if(i != *(rank)){
                MPI_Send( wanna_ponny, sizeof(Packet), MPI_BYTE, i, 0, MPI_COMM_WORLD); //send message about pony suit request 
            }
        }    

        for(int i = 0; i < (*size) - numberOfPonies; i++ ){ //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
            pthread_mutex_lock(&ponySuitMutex);
            pthread_cond_wait(&ponySuitCond, &ponySuitMutex); //wait for signal from listening thread 
            cout<<"Tourist "<<(*rank)<<" got one permission to take pony suit\n";
            pthread_mutex_unlock(&ponySuitMutex);
        }
        //todo
        //serve getting pony suit
        cout<<"Tourist "<<(*rank)<<" got pony suit!\n";

    }
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Status status;
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    pthread_cond_init(&ponySuitCond, NULL);
    pthread_mutex_init(&ponySuitMutex, NULL);

    numberOfPonies = atoi(argv[1]);
    numberOfBoats = atoi(argv[2]);
    maxBoatCapacity = atoi(argv[3]);
    maxVisitorWeight = atoi(argv[4]);

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

    Packet *boats = new Packet[numberOfBoats]; //capacity of each boat 
    if (rank == 0)
    {
        for(int i = 0; i < numberOfBoats; i++)
        {
            boats[i].msgType = BOATS_STATUS_MSG;
            boats[i].capacity = (rand() % maxBoatCapacity)  + 1;
            boats[i].boatOnTrip = false;
        }
        for(int i = 1; i < size; i++)
        {
            MPI_Send(boats, numberOfBoats * sizeof(Packet), MPI_BYTE, i, 0, MPI_COMM_WORLD); //send array with boats capacity
        }        
    }
    else
    {
        MPI_Recv(boats, numberOfBoats * sizeof(Packet), MPI_BYTE, 0, 0, MPI_COMM_WORLD, &status); //wait for boats capacity
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
        MPI_Finalize();
        exit(0);
    }   
    cout<<"Listening thread created - rank: "<<rank<<"\n";
    

    // //create transmitting thread - receiving messages
    // TransmittingThreadData *transmittingThreadData = new TransmittingThreadData;
    // transmittingThreadData->run = &run;
    // pthread_t transmittingThread;
    // if (pthread_create(&transmittingThread, NULL, transmit, (void *) transmittingThreadData))
    // {
    //     MPI_Finalize();
    //     exit(0);
    // }

    //initializatin completed, starting main logic
    visit(&run, boats, ponyQueue, boatQueue, &rank, &size, &status);


    MPI_Finalize();
    delete[] boats;
    delete listeningThreadData;
    // delete transmittingThreadData;
}
