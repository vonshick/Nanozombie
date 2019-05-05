#include <mpi.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <queue>
#include <unistd.h>

#define INITIALIZATION 0
#define WANNA_PONNY 10
#define WANNA_PONNY_RESPONSE 11     //response to 10
//#define TOOK_PONNY_MSG 12           //took pony
#define WANNA_BOAT 20
#define WANNA_BOAT_RESPONSE 21      //response to 20
// #define TOOK_BOAT_MSG 22            //took place on boat
#define BOAT_DEPART 100             //boat departed for trip
#define BOAT_RETURN 101             //boat returned from trip

using namespace std;

const int VISITOR_MAX_WAIT = 10;    //seconds
const int VISITOR_MIN_WAIT = 2;     //seconds

int numberOfPonies;
const int numberOfBoats;
const int maxBoatCapacity;
const int maxVisitorWeight;

pthread_cond_t ponySuitCond;
pthread_mutex_t ponySuitMutex;

struct ListeningThreadData
{
    bool *run;
    int *lamportClock;    
    queue<int> *ponyQueue;
    queue<int> *boatQueue;
    //todo: mutexes for above
};

struct Packet
{
    int msgType; 
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
    MPI_Status status;
    
    while(*(threadData->run))
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status); 
        //check what kind of message came and call proper event
        switch(buffer->msgType){
            case WANNA_PONNY:
                cout<<"Tourist "<< status.MPI_SOURCE <<" want a pony suit!\n";
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

//main thread function, visitor logic
void visit(bool *run, Packet *boats, queue<int> *ponyQueue, queue<int> *boatQueue, int *rank, int *size, int *lamportClock)
{   

    while(*run)
    {
        cout<<"Tourist "<<(*rank)<<" is registered!\n";

        int wait_milisec = (rand() % (VISITOR_MAX_WAIT-VISITOR_MIN_WAIT)*1000) + VISITOR_MIN_WAIT*1000;
        usleep(wait_milisec*1000);

        Packet *wanna_ponny = new Packet;
        (*wanna_ponny).msgType = WANNA_PONNY;

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
    if(argc != 5)
    {
	    cout << "Specify 4 arguments: numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorWeight";
	    exit(0);
    }

    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    numberOfPonies = atoi(argv[1]);
    numberOfBoats = atoi(argv[2]);
    maxBoatCapacity = atoi(argv[3]);
    maxVisitorWeight = atoi(argv[4]);

    //initialization
    //data, variables
    srand(time(NULL));  

    pthread_cond_init(&ponySuitCond, NULL);
    pthread_mutex_init(&ponySuitMutex, NULL); 

    bool run = true;

    Packet *boats = new Packet[numberOfBoats]; //capacity of each boat 
    if (rank == 0)
    {
        for(int i = 0; i < numberOfBoats; i++)
        {
            boats[i].capacity = (rand() % maxBoatCapacity)  + 1;
            boats[i].boatOnTrip = false;
        }
        for(int i = 1; i < size; i++)
        {
            MPI_Send(boats, numberOfBoats * sizeof(Packet), MPI_BYTE, i, INITIALIZATION, MPI_COMM_WORLD); //send array with boats capacity
        }        
    }
    else
    {
        MPI_Recv(boats, numberOfBoats * sizeof(Packet), MPI_BYTE, 0, INITIALIZATION, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //wait for boats capacity
    }

    queue<int> *ponyQueue;  //queue for pony, storing process id
    queue<int> *boatQueue;  //queue for boat
    int lamportClock = 0;   

    //create listening thread - receiving messages
    ListeningThreadData *listeningThreadData = new ListeningThreadData;
    listeningThreadData->run = &run;
    listeningThreadData->lamportClock = &lamportClock;
    listeningThreadData->ponyQueue = ponyQueue;
    listeningThreadData->boatQueue = boatQueue;
    pthread_t listeningThread;

    if (pthread_create(&listeningThread, NULL, listen, (void *) listeningThreadData))
    {
        MPI_Finalize();
        exit(0);
    }   
    cout<<"Listening thread created - rank: "<<rank<<"\n";
    
    //initializatin completed, starting main logic
    visit(&run, boats, ponyQueue, boatQueue, &rank, &size, &lamportClock);

    MPI_Finalize();
    delete[] boats;
    delete listeningThreadData;
}
