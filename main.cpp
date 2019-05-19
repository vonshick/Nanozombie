#include <mpi.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <queue>
#include <unistd.h>

#define IDLE -1
#define INITIALIZATION 0
#define WANNA_PONY 10
#define WANNA_PONY_RESPONSE 11      //response to 10       
#define WANNA_BOAT 20               //has pony, waiting for boat
#define WANNA_BOAT_RESPONSE 21      //response to 20
#define HAS_BOAT_SLOT 22            //has place on boat
#define ON_TRIP 30
#define BOAT_DEPART 100             //boat departed for trip
#define BOAT_RETURN 101             //boat returned from trip

using namespace std;

const int VISITOR_MAX_WAIT = 10;    //seconds
const int VISITOR_MIN_WAIT = 2;     //seconds

pthread_cond_t ponySuitCond;
pthread_mutex_t ponySuitMutex;
pthread_cond_t boatResponseCond;
pthread_mutex_t boatResponseMutex;
pthread_mutex_t lamportMutex;
pthread_mutex_t permissionsMutex;

struct Packet
{
    Packet() {}
    Packet(int cap, bool onTrip, int captId, int lampCl) : capacity(cap), boatOnTrip(onTrip), captainId(captId), lamportClock(lampCl) { }
    int capacity;       
    bool boatOnTrip;   //true - on trip, false - boat in port
    int captainId; //process being captain on current trip
    int lamportClock;
};

struct Boat 
{
    int id;
    int capacity; 
    int capacityLeft;
};

struct BoatSlotRequest
{
    BoatSlotRequest(int idArg, int capacityArg, int lamportClockArg) :  id(idArg), capacity(capacityArg), lamportClock(lamportClockArg) {}
    int id;
    int capacity;
    int lamportClock;
};

struct Data
{
    bool run; 
    int condition;
    int recentRequestClock;
    int necessaryPermissions;
    // Packet boats;    not needed now, idk what is it ~ Piter
    int numberOfPonies;
    int numberOfBoats;
    int maxBoatCapacity;
    int maxVisitorWeight;
    queue<int> ponyQueue;
    queue<int> placeOnBoardQueue;
    Boat* freeBoats;
    Boat* boatsOnTrip;
    Boat currentBoat;
    queue<BoatSlotRequest*> boatQueue; 

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
    Packet *response = new Packet;
    MPI_Status status;
    
    while(data->run)
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status); 
        data->lamportClock = max(data->lamportClock, buffer->lamportClock)+1;
        cout<<data->rank<<": my lamport clock: "<<data->lamportClock<<"\n";
        cout<<data->rank<<": incoming lamport clock: "<<buffer->lamportClock<<"\n";

        switch(status.MPI_TAG){ // check what kind of message came and call proper event
            case WANNA_PONY:
                {    
                    cout<< data->rank <<": "<<"Tourist "<< status.MPI_SOURCE <<" want a pony suit!\n";
                    if (data->condition == WANNA_BOAT){
                        // cout<<data->rank<<": added to pony queue: "<<status.MPI_SOURCE<<"\n";
                        (data->ponyQueue).push(status.MPI_SOURCE);
                    } else if(data->condition == WANNA_PONY && (buffer->lamportClock > data->recentRequestClock || (buffer->lamportClock == data->recentRequestClock && status.MPI_SOURCE > data->rank))){
                        // cout<<data->rank<<": added to pony queue: "<<status.MPI_SOURCE<<"\n";
                        (data->ponyQueue).push(status.MPI_SOURCE);   
                    } else {                
                        MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_PONY_RESPONSE, MPI_COMM_WORLD); // send message about pony suit request 
                        printf("[%d] -> [%d]: sent PONY permission\n", data->rank, status.MPI_SOURCE);                          
                    }
                }
                break;
            case WANNA_PONY_RESPONSE:
                {
                    pthread_mutex_lock(&permissionsMutex);            
                    if(data->necessaryPermissions > 0){
                        pthread_mutex_unlock(&permissionsMutex);            
                        printf("[%d]: received PONY permission from [%d]\n", data->rank, status.MPI_SOURCE);                          
                        pthread_cond_signal(&ponySuitCond);
                    }
                }
                break;
            case WANNA_BOAT:
                    cout<< data->rank <<": "<<"Tourist "<< status.MPI_SOURCE <<" want some place on a boat!\n";
                    pthread_mutex_lock(&lamportMutex);            
                    if(data->condition == WANNA_BOAT && (buffer->lamportClock > data->recentRequestClock || (buffer->lamportClock == data->recentRequestClock && status.MPI_SOURCE > data->rank))){
                        pthread_mutex_unlock(&lamportMutex);         
                        data->placeOnBoardQueue.push(status.MPI_SOURCE);                   
                    } else {                
                        MPI_Send(response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD); // send message about boat permission 
                        printf("[%d] -> [%d]: sent BOAT permission\n", data->rank, status.MPI_SOURCE);                          
                    }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    printf("[%d]: received BOAT response from [%d]\n", data->rank, status.MPI_SOURCE);
                    BoatSlotRequest *boatSlotRequest = new BoatSlotRequest(status.MPI_SOURCE, buffer->capacity, buffer->lamportClock);
                    //todo: remember to delete above!
                    (data->boatQueue).push(boatSlotRequest);
                    if((data->boatQueue).size() == data->size - 1) {    //if all responses received
                        pthread_cond_signal(&boatResponseCond);
                    }
                }
                break;
            default:
                printf("[%d]: WRONG MPI_TAG (%d) FROM [%d]\n", data->rank, status.MPI_TAG, status.MPI_SOURCE);     
                break; 
        }
    }

    delete buffer;
    delete response;
    pthread_exit(NULL);        
}

void prepareToRequest(Data* data, Packet* message, int condition){
    data->condition = condition;
    pthread_mutex_lock(&lamportMutex);
    data->lamportClock += 1; // increment lamportClock before sending pony request
    data->recentRequestClock = data->lamportClock;
    message->lamportClock = data->lamportClock; // send wanna pony request in packet 
    pthread_mutex_unlock(&lamportMutex);
}

//main thread function, visitor logic
void visit(Data *data)
{   
    cout<<"Tourist "<< data->rank <<" is registered!\n";
    Packet *message = new Packet;

    while(data->run)
    {
        int waitMilisec = (rand() % (VISITOR_MAX_WAIT-VISITOR_MIN_WAIT)*1000) + VISITOR_MIN_WAIT*1000;
        usleep(waitMilisec*1000);

        prepareToRequest(data, message, WANNA_PONY);
        for(int i = 0; i < data->size; i++)
        {
            if(i != data->rank){
                MPI_Send(message, sizeof(Packet), MPI_BYTE, i, WANNA_PONY, MPI_COMM_WORLD); //send message about pony suit request 
                printf("[%d] -> [%d]: sent PONY request  (lamport: %d)\n", data->rank, i, message->lamportClock);          
            }
        }    

        data->necessaryPermissions = data->size - data->numberOfPonies; //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
        for(int i = 0; i < data->size - data->numberOfPonies; i++ ){ 
            pthread_mutex_lock(&ponySuitMutex);
            pthread_cond_wait(&ponySuitCond, &ponySuitMutex); //wait for signal from listening thread 
            pthread_mutex_unlock(&ponySuitMutex);
            pthread_mutex_lock(&permissionsMutex);            
            data->necessaryPermissions --;
            pthread_mutex_unlock(&permissionsMutex);
        }

        printf("[%d]: got PONY suit!\n", data->rank);    

        //TODO
        //get slot on boat  
        prepareToRequest(data, message, WANNA_BOAT);
        // send wanna boat request
        for(int i = 0; i < data->size; i++)
        {
            if(i != data->rank){
                MPI_Send( message, sizeof(Packet), MPI_BYTE, i, WANNA_BOAT, MPI_COMM_WORLD); //send message about pony suit request 
                printf("[%d] -> [%d]: sent BOAT request  (lamport: %d)\n", data->rank, i, message->lamportClock);          
            }
        }
        //wait for all answers
        pthread_mutex_lock(&boatResponseMutex);
        pthread_cond_wait(&boatResponseCond, &boatResponseMutex); //wait for signal from listening thread 
        pthread_mutex_unlock(&boatResponseMutex);

        //boarding
        //use data->boatQueue

        //trip

        //free boat slot

        //free your pony suit - send permissions
        int ponyQueueSize = (data->ponyQueue).size();
        for (int i = 0; i < ponyQueueSize; i++){ // send message to all tourists waiting for a pony suit
            MPI_Send( message, sizeof(Packet), MPI_BYTE, (data->ponyQueue).front(), WANNA_PONY_RESPONSE, MPI_COMM_WORLD);
            (data->ponyQueue).pop();
            printf("[%d] -> [%d]: sent PONY permission\n", data->rank, i);
        }

        data->condition = IDLE;
        clearQueue(data->ponyQueue);
        
    }
    delete message;
}

Boat* createBoats(Data* data){
    Boat *boats = new Boat[data->numberOfBoats]; //capacity of each boat 
    if (data->rank == 0)
    {
        for(int i = 0; i < data->numberOfBoats; i++)
        {
            boats[i].capacity = (rand() % data->maxBoatCapacity)  + 1;
            boats[i].capacityLeft = boats[i].capacity;
            // boats[i].boatOnTrip = false;
        }
        for(int i = 1; i < data->size; i++)
        {
            MPI_Send(boats, data->numberOfBoats * sizeof(Boat), MPI_BYTE, i, INITIALIZATION, MPI_COMM_WORLD); //send array with boats capacity
        }        
    }
    else
    {
        MPI_Recv(boats, data->numberOfBoats * sizeof(Boat), MPI_BYTE, 0, INITIALIZATION, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //wait for boats capacity
    }

    return boats;
}

int main(int argc, char **argv)
{
    if(argc != 5)
    {
	    cout << "Specify 4 arguments: numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorWeight\n";
	    exit(0);
    }

    //initialization
    //data, variables

    bool run = true;
    int lamportClock = 0, recentRequestClock = 0, rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    pthread_cond_init(&ponySuitCond, NULL);
    pthread_mutex_init(&ponySuitMutex, NULL); 
    pthread_cond_init(&boatResponseCond, NULL);
    pthread_mutex_init(&boatResponseMutex, NULL);

    Data *data = new Data;
    data->run = run;
    data->condition = IDLE;    
    data->lamportClock = lamportClock;
    data->recentRequestClock = recentRequestClock;
    data->rank = rank; 
    data->size = size;
    data->numberOfPonies = atoi(argv[1]);
    data->numberOfBoats = atoi(argv[2]);
    data->maxBoatCapacity = atoi(argv[3]);
    data->maxVisitorWeight = atoi(argv[4]);
    srand(time(NULL));  
    Boat* boats = createBoats(data);
    data->freeBoats = boats;
    data->currentBoat = boats[0];
    
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
