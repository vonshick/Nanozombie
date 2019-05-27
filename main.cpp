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
#include <bits/stdc++.h>
#include <semaphore.h>

#define IDLE -1
#define INITIALIZATION 0
#define WANNA_PONY 10
#define WANNA_PONY_RESPONSE 11      //response to 10       
#define WANNA_BOAT 20               //has pony, waiting for boat
#define WANNA_BOAT_RESPONSE 21      //response to 20, capacity > 0 means process wants a place on the boat (capacity == 0 - don't want a place)

//TODO: find and do todos :)

//TODO: answers handling when on_board, on_trip?
#define ONBOARD 30
#define ON_TRIP 40
#define END_OF_TRIP 50
#define BOAT_DEPART 100             //boat departed for trip
#define BOAT_SELECT 102             //boat selected as next for boarding

using namespace std;

//seconds:
const int VISITOR_MAX_WAIT = 2;
const int VISITOR_MIN_WAIT = 1;
const int TRIP_MIN_DURATION = 1;
const int TRIP_MAX_DURATION = 2;

sem_t ponyPermissionsSem;
sem_t boatResponsesSem;
sem_t waitForBoatReturnSem;
sem_t waitForBoatSelectSem;
sem_t waitForEndOfTripSem;
sem_t waitForDepartureSem;

// TODO: chcek if they are necessary.. only 2 threads so finf conlicts
pthread_mutex_t lamportMutex;               //CONFIRMED
pthread_mutex_t currentBoatMutex;           //CONFIRMED  //TODO: check if needed with respect to cond mutexes
pthread_mutex_t boatsMutex;                 //CONFIRMED  //check if needed with respect to cond mutexes and check if shoudl be associated with currentBoatMutex
pthread_mutex_t conditionMutex;             //CONFIRMED
pthread_mutex_t recentRequestClockMutex;    //CONFIRMED
pthread_mutex_t boardedBoatMutex;
pthread_mutex_t boardedBoatCapacityMutex;
pthread_mutex_t findingFreeBoatMutex;       //CONFIRMED

struct Packet
{
    Packet() {}
    // Packet(int cap, bool onTrip, int captId, int lampCl) : id(idArg), capacity(cap), boatOnTrip(onTrip), captainId(captId), lamportClock(lampCl) { }
    Packet(int idArg, int cap, bool onTrip, int captId, int lampCl) : boatId(idArg), capacity(cap), captainId(captId), lamportClock(lampCl) { }
    int boatId;
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
    int currentBoat;    //id of current boarding boat. When there were no available boats: -1 if visitor has priority for boarding, //old: -2 if visitor waits further in queue for boarding
    int boardedBoat;            // when visitor is onboard this is boatId. Otherwise = -1
    int boardedBoatCapacity;    // >0 when visitor is captain (this is the capacity of his boat). Otherwise = 0
    int numberOfPonies;
    int numberOfBoats;
    int maxBoatCapacity;
    int maxVisitorCapacity;
    int visitorCapacity;  //capacity of visitor for current trip
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
    int printLamport;
    
    while(data->run)
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        pthread_mutex_lock(&lamportMutex);
        data->lamportClock = max(data->lamportClock, buffer->lamportClock) + 1;
        printLamport = data->lamportClock;
        pthread_mutex_unlock(&lamportMutex);
        // cout<<data->rank<<": my lamport clock: "<<data->lamportClock<<"\n";
        // cout<<data->rank<<": incoming lamport clock: "<<buffer->lamportClock<<"\n";

        switch(status.MPI_TAG) // check what kind of message came and call proper event
        {
            case WANNA_PONY:
                {    
                    printf("[%d]: received WANNA PONY(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&conditionMutex);
                    if (data->condition == WANNA_BOAT)
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        printf("[%d]: added process [%d] WANNA PONY(%d) to PONY QUEUE\n", data->rank, status.MPI_SOURCE, buffer->lamportClock);                          
                        (data->ponyQueue).push(status.MPI_SOURCE);
                    }
                    else
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        if(data->condition == WANNA_PONY && (buffer->lamportClock > data->recentRequestClock || (buffer->lamportClock == data->recentRequestClock && status.MPI_SOURCE > data->rank)))
                        {
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            pthread_mutex_unlock(&conditionMutex);
                            printf("[%d]: added process [%d] WANNA PONY(%d) to PONY QUEUE\n", data->rank, status.MPI_SOURCE, buffer->lamportClock);                          
                            (data->ponyQueue).push(status.MPI_SOURCE);   
                        }
                        else
                        {                
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            pthread_mutex_unlock(&conditionMutex);
                            response->lamportClock = buffer->lamportClock; //let know which request the response concerns (to handle old/unnecessary responses in requesting process)
                            pthread_mutex_lock(&lamportMutex);
                            data->lamportClock += 1;
                            printLamport = data->lamportClock;                   
                            pthread_mutex_unlock(&lamportMutex);
                            MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_PONY_RESPONSE, MPI_COMM_WORLD); // send message about pony suit request 
                            printf("[%d] -> [%d]: sent PONY PERMISSION(%d) (lamport: %d)\n", data->rank, status.MPI_SOURCE, response->lamportClock, printLamport);                          
                        }
                    }
                }
                break;
            case WANNA_PONY_RESPONSE:
                {
                    pthread_mutex_lock(&conditionMutex);
                    pthread_mutex_lock(&recentRequestClockMutex);                    
                    if(data->condition != WANNA_PONY || buffer->lamportClock != data->recentRequestClock)
                    {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        pthread_mutex_unlock(&conditionMutex);                        
                        printf("[%d]: received old PONY PERMISSION(%d) from [%d], skipping. (lamport: %d)\n", data->rank, response->lamportClock, status.MPI_SOURCE, printLamport);
                    }
                    else
                    {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        pthread_mutex_unlock(&conditionMutex);                        
                        data->necessaryPonyPermissions--;
                        if(data->necessaryPonyPermissions == 0)
                        {
                            printf("[%d]: received last required PONY PERMISSION(%d) from [%d] (lamport: %d)\n", data->rank, response->lamportClock, status.MPI_SOURCE, printLamport);
                            sem_post(&ponyPermissionsSem);
                        }
                        else if(data->necessaryPonyPermissions > 0)
                        {
                            printf("[%d]: received PONY PERMISSION(%d) from [%d]. Need %d more. (lamport: %d)\n", data->rank, response->lamportClock, status.MPI_SOURCE, data->necessaryPonyPermissions, printLamport);
                        }
                        else
                        {
                            printf("[%d]: received unnecessary PONY PERMISSION(%d) from [%d]. (lamport: %d)\n",  data->rank, response->lamportClock, status.MPI_SOURCE, printLamport);
                        }
                    }
                }
                break;
            case WANNA_BOAT:
                {
                    printf("[%d]: received WANNA BOAT(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == WANNA_BOAT)
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        response->lamportClock = data->recentRequestClock;
                        pthread_mutex_unlock(&recentRequestClockMutex);     
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = data->visitorCapacity;
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = 0;
                    }
                    pthread_mutex_lock(&lamportMutex);
                    data->lamportClock += 1;
                    printLamport = data->lamportClock;
                    pthread_mutex_unlock(&lamportMutex);
                    MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    if(response->capacity == 0)
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I DON WANT BOAT (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClock, printLamport);                                          
                    }
                    else
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I WANT BOAT(%d): my capacity = %d, (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClock, response->lamportClock, response->capacity, printLamport);                                          
                    }
                }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    if(response->capacity == 0)
                    {
                        printf("[%d]: received WANNA BOAT(%d) response I DON WANT BOAT from [%d] (lamport: %d)\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    }
                    else
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        printf("[%d]: received WANNA BOAT(%d) response I WANT BOAT(%d) from [%d] (lamport: %d)\n", data->rank, data->recentRequestClock, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                        pthread_mutex_unlock(&recentRequestClockMutex);
                    }
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
                        (data->boatRequestList).push_back(boatSlotRequest);
                    }
                    if((data->boatRequestList).size() == data->necessaryBoatAnswers)
                    {    //if all responses received
                        sem_post(&boatResponsesSem);
                    }
                }
                break;
            case BOAT_DEPART:
                {
                    printf("[%d]: received BOAT_DEPART from [%d] (lamport: %d)\n", data->rank, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = 0;
                    pthread_mutex_unlock(&boatsMutex);
                    pthread_mutex_lock(&boardedBoatMutex);
                    if(data->boardedBoat == buffer->boatId)
                    {
                        pthread_mutex_unlock(&boardedBoatMutex);
                        if(data->rank == buffer->captainId) //if visitor is captain
                        {
                            pthread_mutex_lock(&boardedBoatCapacityMutex);
                            data->boardedBoatCapacity = buffer->capacity;
                            pthread_mutex_unlock(&boardedBoatCapacityMutex);

                        }
                        //departing
                        sem_post(&waitForDepartureSem);     //notify waiting visitor   
                    }
                    else
                    {
                        pthread_mutex_unlock(&boardedBoatMutex);
                    }
                }
                break;
            case BOAT_SELECT:
                {
                    printf("[%d]: received BOAT SELECT from [%d] (lamport: %d)\n", data->rank, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&currentBoatMutex);
                    data->currentBoat = buffer->boatId;    //set id of selected boat as current boarding boat
                    pthread_mutex_unlock(&currentBoatMutex);
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = buffer->capacity;
                    pthread_mutex_unlock(&boatsMutex);
                    sem_post(&waitForBoatSelectSem);     //notify waiting visitor    
                    // bool sendSignal = false;
                    // pthread_mutex_lock(&currentBoatMutex);
                    // if(data->currentBoat == -2)
                    // {
                    //     sendSignal = true;
                    // }
                    // data->currentBoat = buffer->boatId;    //set id of selected boat as current boarding boat
                    // pthread_mutex_unlock(&currentBoatMutex);
                    // pthread_mutex_lock(&boatsMutex);
                    // data->boats[buffer->boatId] = buffer->capacity;
                    // pthread_mutex_unlock(&boatsMutex);
                    // if(sendSignal)
                    // {
                    //     sem_post(&waitForBoatSelectSem);     //notify waiting visitor    
                    // }
                }
                break;
            case END_OF_TRIP:
                {
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[data->boardedBoat] = buffer->capacity; // add boat back to the free boats list
                    pthread_mutex_unlock(&boatsMutex);
                    pthread_mutex_lock(&boardedBoatMutex);
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == ON_TRIP && buffer->boatId == data->boardedBoat)
                    {   //if i'm onboard
                        printf("[%d]: TRIP FINISHED! received END OF TRIP from [%d] (lamport: %d)\n", data->rank, status.MPI_SOURCE, printLamport);
                        pthread_mutex_unlock(&conditionMutex);
                        data->boardedBoat = -1;     // get off board
                        pthread_mutex_unlock(&boardedBoatMutex);
                        sem_post(&waitForEndOfTripSem);     //notify waiting visitor    
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        pthread_mutex_unlock(&boardedBoatMutex);
                        printf("[%d]: received END OF TRIP from [%d]. NOT MY TRIP. (lamport: %d)\n", data->rank, status.MPI_SOURCE, printLamport);

                        pthread_mutex_lock(&findingFreeBoatMutex);
                        pthread_mutex_lock(&currentBoatMutex);
                        if(data->currentBoat == -1) //in case when there was no place on any boat for me
                        {
                            data->currentBoat = buffer->boatId;    //set id of boat that returned as current boarding boat
                            pthread_mutex_unlock(&currentBoatMutex);
                            sem_post(&waitForBoatReturnSem);  //notify waiting visitor                      
                        }
                        else
                        {
                            pthread_mutex_unlock(&currentBoatMutex);
                        }
                        pthread_mutex_unlock(&findingFreeBoatMutex);
                    }
                }
                break;
            default:
                printf("[%d]: WRONG MPI_TAG(%d) FROM [%d] (lamport: %d)\n", data->rank, status.MPI_TAG, status.MPI_SOURCE, printLamport);     
                break; 
        }
    }
    delete buffer;
    delete response;
    pthread_exit(NULL);        
}

int setCondition(Data *data, int status)
{
    pthread_mutex_lock(&conditionMutex);
    pthread_mutex_lock(&lamportMutex);
    data->condition = status;
    data->lamportClock += 1;
    int printLamport = data->lamportClock;
    pthread_mutex_unlock(&lamportMutex); 
    pthread_mutex_unlock(&conditionMutex);
    return printLamport;
}

void prepareToRequest(Data *data, Packet *message, int newCondition)
{
    pthread_mutex_lock(&conditionMutex);
    pthread_mutex_lock(&recentRequestClockMutex);
    pthread_mutex_lock(&lamportMutex);
    data->condition = newCondition;
    data->lamportClock += 1; // increment lamportClock before sending request
    data->recentRequestClock = data->lamportClock;
    message->lamportClock = data->recentRequestClock; // set request lamport timestamp in packet 
    pthread_mutex_unlock(&lamportMutex);
    pthread_mutex_unlock(&recentRequestClockMutex);
    pthread_mutex_unlock(&conditionMutex);
    
}

void clearBoatRequestList(Data* data){
    for(int i = 0;i < (data->boatRequestList).size();i++){
        data->boatRequestList[i];
    }
    data->boatRequestList.clear();
}

void findPony(Data *data, Packet *message)
{
    int printLamport;
    data->necessaryPonyPermissions = data->size - data->numberOfPonies; //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
    prepareToRequest(data, message, WANNA_PONY);
    //send wanna pony request
    MPI_Send(message, sizeof(Packet), MPI_BYTE, 0, WANNA_PONY, MPI_COMM_WORLD); //send message about pony suit request 
    printf("[%d] -> [0]: sent PONY(%d) request  (lamport: %d)\n", data->rank, message->lamportClock,  message->lamportClock); 
    for(int i = 1; i < data->size; i++)
    {
        if(i != data->rank)
        {
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            printLamport = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, WANNA_PONY, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent PONY(%d) request  (lamport: %d)\n", data->rank, i, message->lamportClock, printLamport);          
        }
    }    
    //wait for enough permissions
    printf("[%d]: !!! WAITING  !!! for responses to WANNA_PONY(%d)) (lamport: %d)\n", data->rank, message->lamportClock, printLamport); 
    sem_wait(&ponyPermissionsSem);

    pthread_mutex_lock(&lamportMutex);
    printLamport = data->lamportClock;
    pthread_mutex_unlock(&lamportMutex);
    printf("[%d]: !!! WOKEN UP !!! got PONY SUIT! (request WANNA_PONY(%d)) (lamport: %d)\n", data->rank, message->lamportClock, printLamport); 
}

void findFreeBoat(Data *data, int current)
{
    pthread_mutex_lock(&findingFreeBoatMutex);
    for(int i = 0;i < data->numberOfBoats;i++)
    {
        pthread_mutex_lock(&boatsMutex);
        if(data->boats[i] == 0)
        {
            pthread_mutex_unlock(&boatsMutex);
            continue;
        }
        else
        {
            pthread_mutex_unlock(&boatsMutex);
            pthread_mutex_lock(&currentBoatMutex);            
            data->currentBoat = i;
            pthread_mutex_unlock(&currentBoatMutex);
            pthread_mutex_unlock(&findingFreeBoatMutex);
            return;
        }
    }
    //no free boats available now
    pthread_mutex_lock(&currentBoatMutex);
    data->currentBoat = -1; // -1 for visitor with priority for boarding
    pthread_mutex_unlock(&currentBoatMutex);
    pthread_mutex_unlock(&findingFreeBoatMutex);
    printf("[%d]: !!! WAITING  !!! Didn't find free boat to select.\n", data->rank);
    sem_wait(&waitForBoatReturnSem); //wait for listening thread to set first boat that returned as currentBoat
    printf("[%d]: !!! WOKEN UP !!! A boat has returned.\n", data->rank);
    //some boat has returned
    Packet *message = new Packet;
    pthread_mutex_lock(&currentBoatMutex);
    message->boatId = data->currentBoat;
    pthread_mutex_unlock(&currentBoatMutex);
    pthread_mutex_lock(&boatsMutex);
    message->capacity = data->boats[data->currentBoat];
    pthread_mutex_unlock(&boatsMutex);

    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            int printLamport = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);          
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_SELECT, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: sent BOAT SELECT message: boatId: %d, capacity: %d (lamport: %d)\n", data->rank, i, message->boatId, message->capacity, printLamport);
        }
    } 
    delete message;
    return;
}

//passanger action
void waitForEndOfTrip(Data *data)
{
    int printLamport = setCondition(data, ON_TRIP);
    printf("[%d]: IM ON TRIP! (lamport: %d)\n", data->rank, printLamport);
    sem_wait(&waitForEndOfTripSem); //wait for listening thread to receive END OF TRIP
}

//captains action
void manageTheTrip(Data* data)
{
    int printLamport = setCondition(data, ON_TRIP);
    printf("[%d]: IM ON TRIP (AS CAPTAIN)! (lamport: %d)\n", data->rank, printLamport);

    int waitMilisec = (rand() % (TRIP_MAX_DURATION-TRIP_MIN_DURATION)*1000) + TRIP_MIN_DURATION*1000;
    usleep(waitMilisec*1000); // captain decides how much time the trip takes

    Packet *message = new Packet;
    pthread_mutex_lock(&lamportMutex);
    message->lamportClock = data->lamportClock;
    pthread_mutex_unlock(&lamportMutex);
    pthread_mutex_lock(&boardedBoatMutex);
    pthread_mutex_lock(&boardedBoatCapacityMutex);
    //TODO: boatsMutex????
    message->boatId = data->boardedBoat;
    message->capacity = data->boardedBoatCapacity;
    data->boats[data->boardedBoat] = data->boardedBoatCapacity; // add boat back to the list of free boats
    data->boardedBoatCapacity = 0;
    data->boardedBoat = -1;   
    pthread_mutex_unlock(&boardedBoatCapacityMutex);
    pthread_mutex_unlock(&boardedBoatMutex);

    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            int printLamport = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);
            printf("[%d] -> [%d] sent END OF TRIP(%d) message (lamport: %d)\n", data->rank, i, message->lamportClock, printLamport);          
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, END_OF_TRIP, MPI_COMM_WORLD);    //send END OF TRIP message
        }
    }  
}

void getOnBoat(Data *data, int boatId)
{
    pthread_mutex_lock(&lamportMutex);
    int printLamport = data->lamportClock;
    pthread_mutex_unlock(&lamportMutex); 
    printf("[%d]: got on BOAT[%d]! Waiting for BOAT_DEPART... (lamport: %d)\n", data->rank, boatId, printLamport);

    pthread_mutex_lock(&boardedBoatMutex);
    data->boardedBoat = boatId;
    pthread_mutex_unlock(&boardedBoatMutex);

    //wait for departure
    sem_wait(&waitForDepartureSem); //wait for listening thread to receive BOAT_DEPART

    pthread_mutex_lock(&boardedBoatCapacityMutex);
    if(data->boardedBoatCapacity > 0)
    { //if I'm a captain
        pthread_mutex_unlock(&boardedBoatCapacityMutex);
        //trip will begin
        manageTheTrip(data);
        //trip has just ended
    }
    else
    {
        pthread_mutex_unlock(&boardedBoatCapacityMutex);
        //trip will begin
        waitForEndOfTrip(data);
        //trip has just ended
    }
}

void startTrip(Data *data, int departingBoatId, int capacity, int captainId, int startTime)
{
    //TODO: mutex?
    data->boats[departingBoatId] = 0; //mark boat as unavailable (on trip)

    Packet *message = new Packet;
    message->captainId = captainId;
    message->boatId = departingBoatId;
    message->capacity = capacity;
    message->lamportClock = startTime;
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            int printLamport = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_DEPART, MPI_COMM_WORLD);    //send depart message
            printf("[%d] -> [%d]: sent BOAT_DEPART(%d) message: departingBoatId: %d, captainId: %d, capacity: %d (lamport: %d)\n", data->rank, i, startTime, departingBoatId, captainId, capacity, printLamport);          
        }
    }    
    delete message;
}

void placeVisitorsInBoats(Data *data)
{
    pthread_mutex_lock(&currentBoatMutex);
    int boardingBoat = data->currentBoat;
    pthread_mutex_unlock(&currentBoatMutex);    
    pthread_mutex_lock(&boatsMutex);
    int capacityLeft = data->boats[boardingBoat];
    pthread_mutex_unlock(&boatsMutex);    

    int captainId = INT_MAX;
    int boatRequestsNumber = (data->boatRequestList).size();

    int i = 0;
    while(i < boatRequestsNumber) //runs until all visitors with lower lamport clock (higher priority) are placed on boats
    {
        BoatSlotRequest boatSlotRequest = *(data->boatRequestList[i]);
        int visitorCapacity = boatSlotRequest.capacity;
        if(visitorCapacity <= capacityLeft)
        {
            capacityLeft -= visitorCapacity;
            printf("[%d]: FOUND slot on BOAT[%d] for %d element in boats queue: tourist [%d] with lamport %d\n", data->rank, boardingBoat, i, boatSlotRequest.id, boatSlotRequest.lamportClock);
            i++;
        }
        else
        {
            printf("[%d]: DIDN'T FIND slot on BOAT[%d] for %d element in boats queue: tourist [%d] with lamport %d\n", data->rank, boardingBoat, i, boatSlotRequest.id, boatSlotRequest.lamportClock);
            
            // //check if boat has changed in meantime:
            // pthread_mutex_lock(&currentBoatMutex);            
            // if(data->currentBoat == boardingBoat)
            // {
            //     data->currentBoat = -2;
            //     pthread_mutex_unlock(&currentBoatMutex);
            //     //wait for next boat: new value of data->currentBoat, message: BOAT SELECT
            //     printf("[%d]: !!! WAITING  !!! Boarding boat[%d] hasn't changed yet.\n", data->rank, boardingBoat);
            //     sem_wait(&waitForBoatSelectSem); //wait for listening thread to receive BOAT SELECT with next boat
            //     printf("[%d]: !!! WOKEN UP !!! Boarding boat[%d] has changed to [%d].\n", data->rank, boardingBoat, data->currentBoat);
            // }

            //wait for next boat: new value of data->currentBoat, message: BOAT SELECT
            printf("[%d]: !!! WAITING  !!! For boarding boat[%d] change.\n", data->rank, boardingBoat);
            sem_wait(&waitForBoatSelectSem); //wait for listening thread to receive BOAT SELECT with next boat
            printf("[%d]: !!! WOKEN UP !!! Boarding boat[%d] has changed to [%d].\n", data->rank, boardingBoat, data->currentBoat);

            // boat has changed, update loop variables
            pthread_mutex_lock(&currentBoatMutex);            
            boardingBoat = data->currentBoat;
            pthread_mutex_unlock(&currentBoatMutex);
            pthread_mutex_lock(&boatsMutex);
            capacityLeft = data->boats[boardingBoat];
            pthread_mutex_unlock(&boatsMutex);
        }
    }
    //now visitor has the priority for place on boat
    if(data->visitorCapacity > capacityLeft)    //if no place for him, start trip and find next boat for boarding
    {
        pthread_mutex_lock(&lamportMutex);
        int startTime = data->lamportClock;
        pthread_mutex_unlock(&lamportMutex); 
        captainId = (*data->boatRequestList[(data->boatRequestList).size()-1]).id; // last tourist who came into boat becomes a captain
        printf("[%d]: NO PLACE on BOAT[%d] for me! DEPARTING BOAT! (lamport: %d)\n", data->rank, data->currentBoat, startTime);              
        startTrip(data, boardingBoat, data->boats[boardingBoat], captainId, startTime);
        findFreeBoat(data, boardingBoat);
        //after new boat is selected for boarding, he can get onboard because every boat has space for at least one visitor
    }
    clearBoatRequestList(data);

    //TODO: captain should wait for all to be onboard. After findFreeBoat started sending BOAT SELECT, DEPART might be skipped if received before getOnBoat
    // probably need list of passangers

    //get onboard and wait for departure
    getOnBoat(data, data->currentBoat);
    //trip has just ended

    //TODO: what if somebody sits in boat for long after return? is it possible? maybe not: blocking send! check it
    //no, send will proceed as listening thread listens all the time!!
    //so we need synchronization? confirmations?
    // SOLUTION: maybe set mpi tag to receive in listening thread?

}

void findPlaceOnBoat(Data *data,  Packet *message)
{
    data->visitorCapacity = (rand() % data->maxVisitorCapacity) + 1;
    data->necessaryBoatAnswers = data->size - 1;
    
    prepareToRequest(data, message, WANNA_BOAT);

    // send wanna boat request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            int printLamport = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);
            MPI_Send( message, sizeof(Packet), MPI_BYTE, i, WANNA_BOAT, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent WANNA_BOAT(%d) request  (lamport: %d)\n", data->rank, i, message->lamportClock, printLamport);          
        }
    }
    //wait for all answers    
    sem_wait(&boatResponsesSem);

    printf("\n[%d]: UNSORTED LIST:\n", data->rank);          
    for (vector<BoatSlotRequest*>::const_iterator i = (data->boatRequestList).begin(); i != (data->boatRequestList).end(); ++i)
    printf("lamport: [%d] id: [%d] capacity: [%d]\n", (*i)->lamportClock, (*i)->id, (*i)->capacity);          
    printf("==LIST END==\n\n");  

    //sort by lamport the list of candidates for boat
    sort((data->boatRequestList).begin(), (data->boatRequestList).end(), CompareBoatSlotRequest());

    printf("\n[%d]: SORTED LIST:\n", data->rank);          
    for (vector<BoatSlotRequest*>::const_iterator i = (data->boatRequestList).begin(); i != (data->boatRequestList).end(); ++i)
    printf("lamport: [%d] id: [%d] capacity: [%d]\n", (*i)->lamportClock, (*i)->id, (*i)->capacity);          
    printf("==LIST END==\n\n");          

    //find your boat by placing other visitors in boats with regard to lamport
    placeVisitorsInBoats(data);
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
        pthread_mutex_lock(&lamportMutex);
        int printLamport = data->lamportClock;
        pthread_mutex_unlock(&lamportMutex);
        printf("[%d]: I WANT A TRIP! (lamport: %d)\n", data->rank, printLamport);

        //get pony suit
        findPony(data, message);
        
        //find place on boat and get on trip
        findPlaceOnBoat(data, message);
        //here the trip has just ended, but condition = ON_TRIP until pony is freed

        //free your pony suit - send permissions

        //TODO: mutex for queue, condition mutex scope expand?

        int ponyQueueSize = (data->ponyQueue).size();
        for (int i = 0; i < ponyQueueSize; i++)
        { // send message to all tourists waiting for a pony suit
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            printLamport = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);
            MPI_Send( message, sizeof(Packet), MPI_BYTE, (data->ponyQueue).front(), WANNA_PONY_RESPONSE, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: sent queued PONY PERMISSION (lamport: %d)\n", data->rank, i, printLamport);                          
            (data->ponyQueue).pop();
        }
        clearQueue(data->ponyQueue);

        printLamport = setCondition(data, IDLE);
        printf("[%d]: I'M IDLE! (lamport: %d)\n", data->rank, printLamport);
    }
    delete message;
}

void mutexesInit()
{
    pthread_mutex_init(&lamportMutex, NULL);
    pthread_mutex_init(&currentBoatMutex, NULL); 
    pthread_mutex_init(&boatsMutex, NULL); 
    pthread_mutex_init(&conditionMutex, NULL);
    pthread_mutex_init(&recentRequestClockMutex, NULL); 
    pthread_mutex_init(&boardedBoatCapacityMutex, NULL);
    pthread_mutex_init(&boardedBoatMutex, NULL);
    pthread_mutex_init(&findingFreeBoatMutex, NULL);
}

void semaphoresInit()
{
    sem_init(&ponyPermissionsSem, 0, 0);
    sem_init(&boatResponsesSem, 0, 0);
    sem_init(&waitForBoatReturnSem, 0, 0);
    sem_init(&waitForBoatSelectSem, 0, 0);
    sem_init(&waitForEndOfTripSem, 0, 0);
    sem_init(&waitForDepartureSem, 0, 0);
}

int main(int argc, char **argv)
{
    if(argc != 5)
    {
	    cout << "Specify 4 arguments: numberOfPonies, numberOfBoats, maxBoatCapacity, maxVisitorCapacity\n";
	    exit(0);
    }
    if(atoi(argv[1]) < 1 || atoi(argv[2]) < 1 || atoi(argv[3]) < 1 || atoi(argv[4]) < 1)
    {
        cout << "All arguments must be positive integers\n";
	    exit(0);
    }
    if(atoi(argv[3]) <= atoi(argv[4]))  //make sure any boat has space for at least one visitor
    {
	    cout << "maxBoatCapacity must be greater or equal to maxVisitorCapacity\n";
	    exit(0);
    }

    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    mutexesInit();
    semaphoresInit();

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
    data->maxVisitorCapacity = atoi(argv[4]);
    data->boardedBoat = -1;
    data->boardedBoatCapacity = 0;
    data->boats = new int[data->numberOfBoats];
    data->currentBoat = 0;

    srand(time(NULL));      
    if (rank == 0)
    {
        int max = data->maxBoatCapacity;
        int min = data->maxVisitorCapacity;
        for(int i = 0; i < data->numberOfBoats; i++)
        {
            data->boats[i] = (rand() % (max - min + 1)) + min;  //boat capacity in range [maxVisitorCapacity ; maxBoatCapacity]
            printf("Boat[%d] capacity: %d\n", i, data->boats[i]);
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
