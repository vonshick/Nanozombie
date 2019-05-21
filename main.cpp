#include <mpi.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <queue>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <iostream>

#define IDLE -1
#define INITIALIZATION 0
#define WANNA_PONY 10
#define WANNA_PONY_RESPONSE 11      //response to 10       
#define WANNA_BOAT 20               //has pony, waiting for boat
#define WANNA_BOAT_RESPONSE 21      //response to 20, capacity > 0 means process wants a place on the boat (capacity == 0 - don't want a place)
// #define HAS_BOAT_SLOT 22            //has place on boat

//TODO: answers handling when on_trip?
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
pthread_mutex_t ponyPermissionsMutex;
pthread_cond_t waitForFreeBoatCond;
pthread_mutex_t waitForFreeBoatMutex;
pthread_mutex_t currentBoatMutex;
pthread_mutex_t boatsMutex;
pthread_mutex_t conditionMutex;
pthread_mutex_t recentRequestClockMutex;

struct Packet
{
    Packet() {}
    // Packet(int cap, bool onTrip, int captId, int lampCl) : id(idArg), capacity(cap), boatOnTrip(onTrip), captainId(captId), lamportClock(lampCl) { }
    Packet(int idArg, int cap, bool onTrip, int captId, int lampCl) : id(idArg), capacity(cap), captainId(captId), lamportClock(lampCl) { }
    int id;
    int capacity;       
    // bool boatOnTrip;   //true - on trip, false - boat in port
    int captainId; //process being captain on current trip
    int lamportClock;
};

struct BoatSlotRequest
{
    BoatSlotRequest() {}
    BoatSlotRequest(int idArg, int capacityArg, int lamportClockArg) :  id(idArg), capacity(capacityArg), lamportClock(lamportClockArg) {}
    int id;
    int capacity;
    int lamportClock;
};
 
struct Data
{
    bool run; 
    int condition;  //state of visitor: IDLE, WANNA_PONY, WANNA_BOAT, ON_TRIP
    int recentRequestClock;
    int necessaryPonyPermissions;
    int necessaryBoatAnswers;
    int *boats;     //capacity of each boat, 0 - boat is on trip, >0 - boat free. Captain must remember capacity of his boat and send it with boat id in return message
    int currentBoat;    //id of current boat    
    int numberOfPonies;
    int numberOfBoats;
    int maxBoatCapacity;
    int maxVisitorWeight;
    int visitorWeight;  //weight (capacity) of visitor for current trip
    queue<int> ponyQueue;
    vector<BoatSlotRequest*> boatRequestList; //list of requests for place on boat
    int rank; 
    int size; 
    int lamportClock;
};

//for sorting of boatRequestList vector
struct CompareBoatSlotRequest
{
    bool operator()(BoatSlotRequest * lhs, BoatSlotRequest * rhs)
    {
        return lhs->lamportClock < rhs->lamportClock;
    }
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
        data->lamportClock = max(data->lamportClock, buffer->lamportClock) + 1;
        // cout<<data->rank<<": my lamport clock: "<<data->lamportClock<<"\n";
        // cout<<data->rank<<": incoming lamport clock: "<<buffer->lamportClock<<"\n";

        switch(status.MPI_TAG){ // check what kind of message came and call proper event
            case WANNA_PONY:
                {    
                    cout<< data->rank <<": "<<"Tourist "<< status.MPI_SOURCE <<" want a PONY!\n";
                    pthread_mutex_lock(&conditionMutex);
                    if (data->condition == WANNA_BOAT)
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        // cout<<data->rank<<": added to pony queue: "<<status.MPI_SOURCE<<"\n";
                        (data->ponyQueue).push(status.MPI_SOURCE);
                    }
                    else
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        if(data->condition == WANNA_PONY && (buffer->lamportClock > data->recentRequestClock || (buffer->lamportClock == data->recentRequestClock && status.MPI_SOURCE > data->rank)))
                        {
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            pthread_mutex_unlock(&conditionMutex);
                            // cout<<data->rank<<": added to pony queue: "<<status.MPI_SOURCE<<"\n";
                            (data->ponyQueue).push(status.MPI_SOURCE);   
                        }
                        else
                        {                
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            pthread_mutex_unlock(&conditionMutex);
                            MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_PONY_RESPONSE, MPI_COMM_WORLD); // send message about pony suit request 
                            printf("[%d] -> [%d]: sent PONY permission\n", data->rank, status.MPI_SOURCE);                          
                        }
                    }
                }
                break;
            case WANNA_PONY_RESPONSE:
                {
                    pthread_mutex_lock(&ponyPermissionsMutex);            
                    if(data->necessaryPonyPermissions > 0)
                    {
                        pthread_mutex_unlock(&ponyPermissionsMutex);            
                        printf("[%d]: received PONY permission from [%d]\n", data->rank, status.MPI_SOURCE);                          
                        pthread_cond_signal(&ponySuitCond);
                    }
                    else
                    {
                        pthread_mutex_unlock(&ponyPermissionsMutex);            
                    }
                }
                break;
            case WANNA_BOAT:
                {
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == WANNA_BOAT)
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = data->visitorWeight;
                        pthread_mutex_lock(&recentRequestClockMutex);
                        response->lamportClock = data->recentRequestClock;
                        pthread_mutex_unlock(&recentRequestClockMutex);                    
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = 0;
                    }
                    MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    printf("[%d] -> [%d]: sent BOAT response: my weight = %d, lamport = %d\n", data->rank, status.MPI_SOURCE, response->capacity, response->lamportClock);                                          
                }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    printf("[%d]: received BOAT response from [%d]\n", data->rank, status.MPI_SOURCE);
                    pthread_mutex_lock(&recentRequestClockMutex);
                    if(buffer->capacity == 0 || buffer->lamportClock > data->recentRequestClock) {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        //if process don't want a place on boat or place request is older - don't queue up his answer (decrease required number of answers in queue)
                        data->necessaryBoatAnswers--;
                    }
                    else
                    {    //queue up the answer for place on boat
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        BoatSlotRequest *boatSlotRequest = new BoatSlotRequest(status.MPI_SOURCE, buffer->capacity, buffer->lamportClock);
                        //todo: remember to delete above!
                        (data->boatRequestList).push_back(boatSlotRequest);
                        if((data->boatRequestList).size() == data->necessaryBoatAnswers)
                        {    //if all responses received
                            pthread_cond_signal(&boatResponseCond);
                        }
                    }
                }
                break;
            case BOAT_RETURN:
                {
                    //TODO
                    //notify passangers
                    
                    //mark a boat as free. If there had been no free boats for boarding - wake up waiting thread
                    pthread_mutex_lock(&currentBoatMutex);
                    if(data->currentBoat == -1)
                    {
                        data->currentBoat = buffer->id;    //set id of boat that returned as current boarding boat
                        pthread_mutex_unlock(&currentBoatMutex);
                        data->boats[buffer->id] = buffer->capacity;
                        pthread_cond_signal(&waitForFreeBoatCond);               
                    }
                    else
                    {
                        pthread_mutex_unlock(&currentBoatMutex);                       
                    }
                }
                break;
            case BOAT_DEPART:
                {
                    //TODO
                    //wait for return... hmm anything else?
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

void prepareToRequest(Data *data, Packet *message, int newCondition)
{
    pthread_mutex_lock(&conditionMutex);
    data->condition = newCondition;
    pthread_mutex_unlock(&conditionMutex);
    
    pthread_mutex_lock(&lamportMutex);
    data->lamportClock += 1; // increment lamportClock before sending pony request
    pthread_mutex_lock(&recentRequestClockMutex);
    data->recentRequestClock = data->lamportClock;
    pthread_mutex_unlock(&recentRequestClockMutex);
    message->lamportClock = data->lamportClock; // send wanna pony request in packet 
    pthread_mutex_unlock(&lamportMutex);
}

void findPony(Data *data, Packet *message)
{
    prepareToRequest(data, message, WANNA_PONY);
    //send wanna pony request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, WANNA_PONY, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent PONY request  (lamport: %d)\n", data->rank, i, message->lamportClock);          
        }
    }    
    //wait for enough permissions
    data->necessaryPonyPermissions = data->size - data->numberOfPonies; //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
    for(int i = 0; i < data->size - data->numberOfPonies; i++ )
    { 
        pthread_mutex_lock(&ponySuitMutex);
        pthread_cond_wait(&ponySuitCond, &ponySuitMutex); //wait for signal from listening thread 
        pthread_mutex_unlock(&ponySuitMutex);
        pthread_mutex_lock(&ponyPermissionsMutex);            
        data->necessaryPonyPermissions--;
        pthread_mutex_unlock(&ponyPermissionsMutex);
    }

    printf("[%d]: got PONY suit!\n", data->rank); 
}

void findFreeBoat(Data *data, int current)
{
    for(int i = 0;i < data->numberOfBoats;i++)
    {
        pthread_mutex_lock(&boatsMutex);
        if(data->boats[i] == 0 || i == current)
        {
            pthread_mutex_unlock(&boatsMutex);
            continue;
        }
        else
        {
            pthread_mutex_unlock(&boatsMutex);
            data->currentBoat = i;
            return;
        }
    }
    //no free boats available now
    pthread_mutex_lock(&currentBoatMutex);
    data->currentBoat = -1;
    pthread_mutex_unlock(&currentBoatMutex);
    pthread_mutex_lock(&waitForFreeBoatMutex);
    pthread_cond_wait(&waitForFreeBoatCond, &waitForFreeBoatMutex); //wait for listening thread to set first boat that returned as currentBoat
    pthread_mutex_unlock(&waitForFreeBoatMutex);
    return;
}

void endTrip()
{
    //captains responsibility
}

void startTrip(Data *data, int departingBoatId, int capacity)
{
    Packet *message = new Packet;
    // message->captainId = captainId;      TODO
    message->id = departingBoatId;
    message->capacity = capacity;
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_DEPART, MPI_COMM_WORLD);    //send depart message
        }
    }    
    data->boats[data->currentBoat] = 0;
    delete message;
}

void placeVisitorsInBoats(Data *data)
{
    int capacityLeft = data->boats[data->currentBoat];
    // TODO     int captainId;
    for(int i = 0;i < (data->boatRequestList).size();i++) //runs until all visitors with lower lamport clock (higher priority) are placed on boats
    {
        BoatSlotRequest boatSlotRequest = *(data->boatRequestList[i]);
        int visitorCapacity = boatSlotRequest.capacity;
        if(visitorCapacity <= capacityLeft)
        {
            capacityLeft -= visitorCapacity;
        }
        else
        {
            startTrip(data, data->currentBoat, data->boats[data->currentBoat]);
            findFreeBoat(data, data->currentBoat);
            capacityLeft = data->boats[data->currentBoat];
        }
        
    }
    //now visitor has the priority for place on boat

    //TODO: getgot place, wait for depart
}

void findPlaceOnBoat(Data *data,  Packet *message)
{
    data->visitorWeight = (rand() % data->maxVisitorWeight) + 1;
    prepareToRequest(data, message, WANNA_BOAT);
    // send wanna boat request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            MPI_Send( message, sizeof(Packet), MPI_BYTE, i, WANNA_BOAT, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent BOAT request  (lamport: %d)\n", data->rank, i, message->lamportClock);          
        }
    }
    //wait for all answers    
    data->necessaryBoatAnswers = data->size - 1;
    pthread_mutex_lock(&boatResponseMutex);
    pthread_cond_wait(&boatResponseCond, &boatResponseMutex); //wait for signal from listening thread 
    pthread_mutex_unlock(&boatResponseMutex);
    //sort by lamport the list of candidates for boat
    sort((data->boatRequestList).begin(), (data->boatRequestList).end(), CompareBoatSlotRequest());
    //find your boat by placing other visitors in boats with regard to lamport
    placeVisitorsInBoats(data);

    int boat;
    printf("[%d]: got on BOAT[%d]!\n", data->rank, boat);      
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
        printf("[%d]: I WANT A TRIP!\n", data->rank);

        //get pony suit
        findPony(data, message);
        
        //find place on boat
        findPlaceOnBoat(data, message);

        //trip
        pthread_mutex_lock(&conditionMutex);
        data->condition = ON_TRIP;
        pthread_mutex_unlock(&conditionMutex);
        
        //TODO: notify

        //end of trip
        //TODO: notify

        //free your pony suit - send permissions
        int ponyQueueSize = (data->ponyQueue).size();
        for (int i = 0; i < ponyQueueSize; i++)
        { // send message to all tourists waiting for a pony suit
            MPI_Send( message, sizeof(Packet), MPI_BYTE, (data->ponyQueue).front(), WANNA_PONY_RESPONSE, MPI_COMM_WORLD);
            (data->ponyQueue).pop();
            printf("[%d] -> [%d]: sent PONY permission\n", data->rank, i);
        }

        pthread_mutex_lock(&conditionMutex);
        data->condition = IDLE;
        pthread_mutex_unlock(&conditionMutex);

        clearQueue(data->ponyQueue);
        printf("[%d]: I GO TO SLEEP!\n", data->rank);
    }
    delete message;
}

int main(int argc, char **argv)
{
    if(argc != 5)
    {
	    cout << "Specify 4 arguments: numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorWeight\n";
	    exit(0);
    }

    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    pthread_cond_init(&ponySuitCond, NULL);
    pthread_mutex_init(&ponySuitMutex, NULL); 
    pthread_cond_init(&boatResponseCond, NULL);
    pthread_mutex_init(&boatResponseMutex, NULL);
    pthread_mutex_init(&lamportMutex, NULL);
    pthread_mutex_init(&ponyPermissionsMutex, NULL);
    pthread_cond_init(&waitForFreeBoatCond, NULL);
    pthread_mutex_init(&waitForFreeBoatMutex, NULL); 
    pthread_mutex_init(&currentBoatMutex, NULL); 
    pthread_mutex_init(&boatsMutex, NULL); 
    pthread_mutex_init(&conditionMutex, NULL);
    pthread_mutex_init(&recentRequestClockMutex, NULL); 

    queue<int> ponyQueue;  //queue for pony, storing process id
    vector<BoatSlotRequest*> boatRequestList;  //queue for boat place requests

    Data *data = new Data;
    data->run = true;
    data->condition = IDLE;    
    data->lamportClock = 0;
    data->recentRequestClock = 0;
    data->rank = rank; 
    data->size = size;
    data->ponyQueue = ponyQueue;
    data->boatRequestList = boatRequestList;
    data->numberOfPonies = atoi(argv[1]);
    data->numberOfBoats = atoi(argv[2]);
    data->maxBoatCapacity = atoi(argv[3]);
    data->maxVisitorWeight = atoi(argv[4]);
    data->boats = new int[data->numberOfBoats];
    data->currentBoat = 0;
    srand(time(NULL));      
    if (rank == 0)
    {
        for(int i = 0; i < data->numberOfBoats; i++)
        {
            data->boats[i] = (rand() % data->maxBoatCapacity)  + 1;
        }
        for(int i = 1; i < size; i++)
        {
            MPI_Send(data->boats, data->numberOfBoats, MPI_INT, i, INITIALIZATION, MPI_COMM_WORLD); //send array with boats capacity
        }        
    }
    else
    {
        MPI_Recv(data->boats, data->numberOfBoats, MPI_INT, 0, INITIALIZATION, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //wait for boats capacity
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
    delete[] data->boats;
    delete data;

    return 0;
}
