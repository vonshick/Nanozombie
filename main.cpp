#include <mpi.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <queue>
#include <unistd.h>

#define INITIALIZATION 0
#define WANNA_PONY 10
#define WANNA_PONY_RESPONSE 11     //response to 10
//#define TOOK_PONY_MSG 12           //took pony
#define WANNA_BOAT 20
#define WANNA_BOAT_RESPONSE 21      //response to 20
// #define TOOK_BOAT_MSG 22            //took place on boat
#define BOAT_DEPART 100             //boat departed for trip
#define BOAT_RETURN 101             //boat returned from trip

using namespace std;

const int VISITOR_MAX_WAIT = 10;    //seconds
const int VISITOR_MIN_WAIT = 2;     //seconds

pthread_cond_t ponySuitCond;
pthread_mutex_t ponySuitMutex;

struct Packet
{
    Packet() {}
    Packet(int msgT, int cap, bool onTrip, int captId, int lampCl) : msgType(msgT), capacity(cap), boatOnTrip(onTrip), captainId(captId), lamportClock(lampCl) { }
    int msgType; 
    int capacity;       
    bool boatOnTrip;   //true - on trip, false - boat in port
    int captainId; //process being captain on current trip
    int lamportClock;
};

struct Data
{
    bool run; 
    int condition;
    Packet boats;
    int numberOfPonies;
    int numberOfBoats;
    int maxBoatCapacity;
    int maxVisitorWeight;
    queue<int> ponyQueue;
    queue<int> boatQueue; 
    int rank; 
    int size; 
    int lamportClock;
};

void clearQueue( queue<int> &q )
{
   queue<int> empty;
   swap( q, empty );
}

//function for listening thread - receiving messages
void *listen(void *voidData)
{
    pthread_detach(pthread_self());
    Data *data = (Data *) voidData;
    Packet *buffer = new Packet;
    Packet *response = new Packet(-1, 0, 0, 0, 0);
    MPI_Status status;
    // int *lamportClock = &(data->lamportClock); 
    
    while(data->run)
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status); 
        //check what kind of message came and call proper event
        switch(buffer->msgType){
            case WANNA_PONY:
                cout<< data->rank <<": "<<"Tourist "<< status.MPI_SOURCE <<" want a pony suit!\n";
                // if( data->condition != WANNA_PONY || buffer->lamportClock < data->lamportClock){ //if thread doesn't want o pony suit or it requested for that later than thread sending package
                //     response->msgType = WANNA_PONY_RESPONSE;
                //     // response->lamportClock                    
                //     MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, 0, MPI_COMM_WORLD); //send message about pony suit request 
                // } else {
                //     (data->ponyQueue).push(status.MPI_SOURCE);
                // }
                pthread_cond_signal(&ponySuitCond); //that's only for now - to test pthread_cond_signal
                break;
            case WANNA_PONY_RESPONSE:
                pthread_cond_signal(&ponySuitCond); //that's only for now - to test pthread_cond_signal

            default:
                break; 
        }
    }

    delete buffer;
    delete response;
    pthread_exit(NULL);        
}


//main thread function, visitor logic
void visit(Data *data)
{   
    cout<<"Tourist "<< data->rank <<" is registered!\n";

    while(data->run)
    {
        int waitMilisec = (rand() % (VISITOR_MAX_WAIT-VISITOR_MIN_WAIT)*1000) + VISITOR_MIN_WAIT*1000;
        usleep(waitMilisec*1000);

        // *(data->lamportClock) += 1; // increment lamportClock before sending message
        data->condition = WANNA_PONY;
        (data->ponyQueue).push(data->rank); // push yourself to pony suit queue 
        Packet *message = new Packet(WANNA_PONY, 0, 0, 0, data->lamportClock); // and send it in packet 
        for(int i = 0; i < data->size; i++)
        {
            if(i != data->rank){
                MPI_Send( message, sizeof(Packet), MPI_BYTE, i, 0, MPI_COMM_WORLD); //send message about pony suit request 
            }
        }    


        for(int i = 0; i < data->size - data->numberOfPonies; i++ ){ //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
            pthread_mutex_lock(&ponySuitMutex);
            pthread_cond_wait(&ponySuitCond, &ponySuitMutex); //wait for signal from listening thread 
            cout<<"Tourist "<<data->rank<<" got one permission to take pony suit\n";
            pthread_mutex_unlock(&ponySuitMutex);
        }
        for (int i = 0; i < (data->ponyQueue).size(); i++){
            message->msgType = WANNA_PONY_RESPONSE;                    
            MPI_Send( message, sizeof(Packet), MPI_BYTE, i, 0, MPI_COMM_WORLD); //send message about pony suit request 
        } 
        cout<<"Tourist "<< data->rank <<" got pony suit!\n";
        clearQueue(data->ponyQueue);
        
        delete message;
        Packet *wannaBoat = new Packet;

    }
}

int main(int argc, char **argv)
{
    if(argc != 5)
    {
	    cout << "Specify 4 arguments: numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorWeight";
	    exit(0);
    }

    //initialization
    //data, variables

    bool run = true;
    int lamportClock = 0, rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    pthread_cond_init(&ponySuitCond, NULL);
    pthread_mutex_init(&ponySuitMutex, NULL); 

    queue<int> ponyQueue;  //queue for pony, storing process id
    queue<int> boatQueue;  //queue for boat

    Data *data = new Data;
    data->run = run;
    data->lamportClock = lamportClock;
    data->rank = rank; 
    data->size = size;
    data->ponyQueue = ponyQueue;
    data->boatQueue = boatQueue;
    data->numberOfPonies = atoi(argv[1]);
    data->numberOfBoats = atoi(argv[2]);
    data->maxBoatCapacity = atoi(argv[3]);
    data->maxVisitorWeight = atoi(argv[4]);

    srand(time(NULL));  

    Packet *boats = new Packet[data->numberOfBoats]; //capacity of each boat 
    if (rank == 0)
    {
        data->lamportClock = 1;
        for(int i = 0; i < data->numberOfBoats; i++)
        {
            boats[i].capacity = (rand() % data->maxBoatCapacity)  + 1;
            boats[i].boatOnTrip = false;
        }
        for(int i = 1; i < size; i++)
        {
            MPI_Send(boats, data->numberOfBoats * sizeof(Packet), MPI_BYTE, i, INITIALIZATION, MPI_COMM_WORLD); //send array with boats capacity
        }        
    }
    else
    {
        MPI_Recv(boats, data->numberOfBoats * sizeof(Packet), MPI_BYTE, 0, INITIALIZATION, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //wait for boats capacity
        data->lamportClock = 2;
    }

    //create listening thread - receiving messages
    pthread_t listeningThread;
    if (pthread_create(&listeningThread, NULL, listen, (void *) data))
    {
        MPI_Finalize();
        exit(0);
    }   
    cout<<"Listening thread created - rank: "<<rank<<"\n";
    
    //initializatin completed, starting main logic
    visit(data);

    MPI_Finalize();
    delete[] boats;
    delete data;
}
