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
// #define BOAT_RETURN 101             //boat returned from trip
#define BOAT_SELECT 102             //boat selected as next for boarding

using namespace std;

const int VISITOR_MAX_WAIT = 2;    //seconds
const int VISITOR_MIN_WAIT = 1;     //seconds
const int TRIP_MIN_DURATION = 1;
const int TRIP_MAX_DURATION = 2;


// TODO: chcek if they are necessary.. only 2 threads so finf conlicts
pthread_cond_t ponySuitCond;
pthread_mutex_t ponySuitMutex;
pthread_cond_t boatResponseCond;
pthread_mutex_t boatResponseMutex;
pthread_mutex_t lamportMutex;
pthread_mutex_t ponyPermissionsMutex;
pthread_cond_t waitForFreeBoatCond;
pthread_mutex_t waitForFreeBoatMutex;
pthread_mutex_t currentBoatMutex;   //check if needed with respect to cond mutexes
pthread_mutex_t boatsMutex;         ////check if needed with respect to cond mutexes and check if shoudl be associated with currentBoatMutex
pthread_mutex_t conditionMutex;
pthread_mutex_t recentRequestClockMutex;
pthread_cond_t waitForDepartureCond;
pthread_mutex_t waitForDepartureMutex;
pthread_cond_t waitForEndOfTripCond;
pthread_mutex_t waitForEndOfTripMutex;
pthread_mutex_t boatRequestListMutex;
pthread_mutex_t boardedBoatMutex;
pthread_mutex_t boardedBoatCapacityMutex;
pthread_mutex_t necessaryBoatAnswersMutex;

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
    int condition;  //state of visitor: IDLE, WANNA_PONY, WANNA_BOAT, ONBOARD, ON_TRIP
    int recentRequestClock;
    int necessaryPonyPermissions;
    int necessaryBoatAnswers;
    int *boats;     //capacity of each boat, 0 - boat is on trip, >0 - boat free. Captain must remember capacity of his boat and send it with boat id in return message
    int currentBoat;    //id of current boarding boat. When there were no available boats: -1 if visitor has priority for boarding, -2 if visitor waits further in queue for boarding
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
    
    while(data->run)
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status); 
        data->lamportClock = max(data->lamportClock, buffer->lamportClock) + 1;
        // cout<<data->rank<<": my lamport clock: "<<data->lamportClock<<"\n";
        // cout<<data->rank<<": incoming lamport clock: "<<buffer->lamportClock<<"\n";

        switch(status.MPI_TAG) // check what kind of message came and call proper event
        {
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
                            pthread_mutex_lock(&lamportMutex);
                            data->lamportClock += 1;
                            pthread_mutex_unlock(&lamportMutex);
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
                        pthread_mutex_lock(&ponySuitMutex);
                        pthread_cond_signal(&ponySuitCond);
                        pthread_mutex_unlock(&ponySuitMutex);                                              
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
                        response->capacity = data->visitorCapacity;
                        pthread_mutex_lock(&recentRequestClockMutex);
                        response->lamportClock = data->recentRequestClock;
                        pthread_mutex_unlock(&recentRequestClockMutex);                    
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = 0;
                    }
                    pthread_mutex_lock(&lamportMutex);
                    data->lamportClock += 1;
                    pthread_mutex_unlock(&lamportMutex);
                    MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    printf("[%d] -> [%d]: sent BOAT response: my capacity = %d, lamport = %d\n", data->rank, status.MPI_SOURCE, response->capacity, response->lamportClock);                                          
                }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    printf("[%d]: received BOAT response from [%d]\n", data->rank, status.MPI_SOURCE);
                    pthread_mutex_lock(&recentRequestClockMutex);
                    if(buffer->capacity == 0 || buffer->lamportClock > data->recentRequestClock) {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        //if process don't want a place on boat or place request is older - don't queue up his answer (decrease required number of answers in queue)
                        pthread_mutex_lock(&necessaryBoatAnswersMutex);                    
                        data->necessaryBoatAnswers--;
                        pthread_mutex_unlock(&necessaryBoatAnswersMutex);                    
                    }
                    else
                    {    //queue up the answer for place on boat
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        BoatSlotRequest *boatSlotRequest = new BoatSlotRequest(status.MPI_SOURCE, buffer->capacity, buffer->lamportClock);

                        //TODO: check for dead lock (theres  one more tripple mutex lock somwhere - find an check)

                        pthread_mutex_lock(&boatResponseMutex);
                        pthread_mutex_lock(&boatRequestListMutex);                    
                        (data->boatRequestList).push_back(boatSlotRequest);
                        pthread_mutex_lock(&necessaryBoatAnswersMutex); 
                        if((data->boatRequestList).size() == data->necessaryBoatAnswers)
                        {    //if all responses received
                            pthread_mutex_unlock(&necessaryBoatAnswersMutex);                    
                            pthread_mutex_unlock(&boatRequestListMutex);  
                            pthread_cond_signal(&boatResponseCond);
                            pthread_mutex_unlock(&boatResponseMutex);
                        }
                        else
                        {
                            pthread_mutex_unlock(&necessaryBoatAnswersMutex);                    
                            pthread_mutex_unlock(&boatRequestListMutex);  
                            pthread_mutex_unlock(&boatResponseMutex);
                        }
                    }
                }
                break;
            case BOAT_DEPART:
                {
                    printf("[%d]: received BOAT_DEPART from [%d]\n", data->rank, status.MPI_SOURCE);
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = 0;
                    pthread_mutex_unlock(&boatsMutex);
                    pthread_mutex_lock(&waitForDepartureMutex);
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
                        pthread_cond_signal(&waitForDepartureCond);     //notify waiting visitor   
                        pthread_mutex_unlock(&waitForDepartureMutex);           
                    }
                    else
                    {
                        pthread_mutex_unlock(&boardedBoatMutex);
                        pthread_mutex_unlock(&waitForDepartureMutex);           
                    }
                }
                break;
            case BOAT_SELECT:
                {
                    printf("[%d]: received BOAT_SELECT from [%d]\n", data->rank, status.MPI_SOURCE);
                    bool sendSignal = false;
                    pthread_mutex_lock(&waitForFreeBoatMutex);
                    pthread_mutex_lock(&currentBoatMutex);
                    if(data->currentBoat == -2)
                    {
                        sendSignal = true;
                    }
                    data->currentBoat = buffer->boatId;    //set id of selected boat as current boarding boat
                    pthread_mutex_unlock(&currentBoatMutex);
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = buffer->capacity;
                    pthread_mutex_unlock(&boatsMutex);
                    if(sendSignal)
                    {
                        pthread_cond_signal(&waitForFreeBoatCond);     //notify waiting visitor    
                    }
                    pthread_mutex_unlock(&waitForFreeBoatMutex);
                }
                break;
            case END_OF_TRIP:
                {
                    printf("[%d]: received END_OF_TRIP from [%d]\n", data->rank, status.MPI_SOURCE);
                    pthread_mutex_lock(&waitForEndOfTripMutex);                    
                    pthread_mutex_lock(&boardedBoatMutex);
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == ON_TRIP && buffer->boatId == data->boardedBoat)
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        pthread_mutex_lock(&boatsMutex);
                        data->boats[data->boardedBoat] = buffer->capacity; // add boat back to the free boats list
                        pthread_mutex_unlock(&boatsMutex);
                        data->boardedBoat = -1;     // get off board
                        pthread_mutex_unlock(&boardedBoatMutex);
                        pthread_cond_signal(&waitForEndOfTripCond);     //notify waiting visitor    
                        pthread_mutex_unlock(&waitForEndOfTripMutex);
                    }
                    else
                        pthread_mutex_unlock(&conditionMutex);
                        pthread_mutex_unlock(&boardedBoatMutex);
                        pthread_mutex_unlock(&waitForEndOfTripMutex);
                    }
                    pthread_mutex_lock(&waitForFreeBoatMutex);
                    pthread_mutex_lock(&currentBoatMutex);
                    if(data->currentBoat == -1) //in case when there was no place on any boat for me
                    {
                        data->currentBoat = buffer->boatId;    //set id of boat that returned as current boarding boat
                        pthread_mutex_unlock(&currentBoatMutex);
                        pthread_cond_signal(&waitForFreeBoatCond);  //notify waiting visitor                      
                        pthread_mutex_unlock(&waitForFreeBoatMutex);
                    }
                    else
                    {
                        pthread_mutex_unlock(&currentBoatMutex);
                        pthread_mutex_unlock(&waitForFreeBoatMutex);
                    }
                    //TODO anything else?                 
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
    data->lamportClock += 1; // increment lamportClock before sending request
    pthread_mutex_lock(&recentRequestClockMutex);
    data->recentRequestClock = data->lamportClock;
    message->lamportClock = data->recentRequestClock; // set request lamport timestamp in packet 
    pthread_mutex_unlock(&recentRequestClockMutex);
    pthread_mutex_unlock(&lamportMutex);
}

void clearBoatRequestList(Data* data){
    pthread_mutex_lock(&boatRequestListMutex);
    for(int i = 0;i < (data->boatRequestList).size();i++){
        data->boatRequestList[i];
    }
    data->boatRequestList.clear();
    pthread_mutex_unlock(&boatRequestListMutex);
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
    pthread_mutex_lock(&ponyPermissionsMutex);            
    data->necessaryPonyPermissions = data->size - data->numberOfPonies; //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
    pthread_mutex_unlock(&ponyPermissionsMutex);            
    
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
    pthread_mutex_lock(&waitForFreeBoatMutex);
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
            pthread_mutex_lock(&currentBoatMutex);            
            data->currentBoat = i;
            pthread_mutex_unlock(&currentBoatMutex);
            pthread_mutex_unlock(&waitForFreeBoatMutex);
            return;
        }
    }
    //no free boats available now
    pthread_mutex_lock(&currentBoatMutex);
    data->currentBoat = -1; // -1 for visitor with priority for boarding
    pthread_mutex_unlock(&currentBoatMutex);
    pthread_cond_wait(&waitForFreeBoatCond, &waitForFreeBoatMutex); //wait for listening thread to set first boat that returned as currentBoat
    pthread_mutex_unlock(&waitForFreeBoatMutex);
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
            printf("[%d] -> [%d]: sent BOAT_SELECT message  (boatId: %d, capacity: %d)\n", data->rank, i, message->boatId, message->capacity);
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            pthread_mutex_unlock(&lamportMutex);          
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_SELECT, MPI_COMM_WORLD);
        }
    } 
    delete message;
    return;
}

//passanger action
void waitForEndOfTrip(Data *data)
{
    printf("[%d]: IM ON TRIP!\n", data->rank);
    pthread_mutex_lock(&waitForEndOfTripMutex);
    pthread_mutex_lock(&conditionMutex);
    data->condition = ON_TRIP;
    pthread_mutex_unlock(&conditionMutex);
    pthread_cond_wait(&waitForEndOfTripCond, &waitForEndOfTripMutex); //wait for listening thread to receive END_OF_TRIP
    pthread_mutex_unlock(&waitForEndOfTripMutex);
}

//captains action
void manageTheTrip(Data* data)
{
    printf("[%d]: IM ON TRIP (AS CAPTAIN)!\n", data->rank);

    pthread_mutex_lock(&conditionMutex);
    data->condition = ON_TRIP;
    pthread_mutex_unlock(&conditionMutex);

    int waitMilisec = (rand() % (TRIP_MAX_DURATION-TRIP_MIN_DURATION)*1000) + TRIP_MIN_DURATION*1000;
    usleep(waitMilisec*1000); // captain decides how much time the trip takes

    Packet *message = new Packet;
    pthread_mutex_lock(&boardedBoatMutex);
    pthread_mutex_lock(&boardedBoatCapacityMutex);
    message->boatId = data->boardedBoat;
    message->capacity = data->boardedBoatCapacity;
    data->boats[data->boardedBoat] = data->boardedBoatCapacity; // add boat back to the list of free boats
    data->boardedBoatCapacity = 0;
    data->boardedBoat = -1;   
    pthread_mutex_unlock(&boardedBoatCapacityMutex);
    pthread_mutex_unlock(&boardedBoatMutex);

    int lamportLocal;
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            lamportLocal = data->lamportClock;
            pthread_mutex_unlock(&lamportMutex);

            //TODO: prints with lamport everywhere

            printf("[%d] -> [%d] (%d): sent END OF TRIP message \n", data->rank, i, lamportLocal);          
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, END_OF_TRIP, MPI_COMM_WORLD);    //send depart message
        }
    }  
}

void getOnBoat(Data *data, int boatId)
{
    printf("[%d]: got on BOAT[%d]!\n", data->rank, boatId);
    pthread_mutex_lock(&waitForDepartureMutex);
    pthread_mutex_lock(&boardedBoatMutex);
    data->boardedBoat = boatId;
    pthread_mutex_unlock(&boardedBoatMutex);
    pthread_mutex_lock(&conditionMutex);
    data->condition = ONBOARD;
    pthread_mutex_unlock(&conditionMutex);
    //wait for departure
    pthread_cond_wait(&waitForDepartureCond, &waitForDepartureMutex); //wait for listening thread to receive BOAT_DEPART
    pthread_mutex_unlock(&waitForDepartureMutex);

    pthread_mutex_lock(&boardedBoatCapacityMutex);

    //TODO: captain should board last and start trip only when everybody is onboard!

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

void startTrip(Data *data, int departingBoatId, int capacity, int captainId)
{
    Packet *message = new Packet;
    message->captainId = captainId;
    message->boatId = departingBoatId;
    message->capacity = capacity;
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            printf("[%d] -> [%d]: sent DEPART message  (departingBoatId: %d, captainId: %d, capacity: %d)\n", data->rank, i, departingBoatId, captainId, capacity);          
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            pthread_mutex_unlock(&lamportMutex);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_DEPART, MPI_COMM_WORLD);    //send depart message
        }
    }    
    data->boats[departingBoatId] = 0; //mark boat as unavailable (on trip)
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

    pthread_mutex_lock(&boatRequestListMutex);
    int boatRequestsNumber =  (data->boatRequestList).size();
    pthread_mutex_unlock(&boatRequestListMutex);

    for(int i = 0;i < boatRequestsNumber;i++) //runs until all visitors with lower lamport clock (higher priority) are placed on boats
    {
        BoatSlotRequest boatSlotRequest = *(data->boatRequestList[i]);
        printf("[%d] %d element in queue: tourist %d lamport %d\n", data->rank, i, boatSlotRequest.id, boatSlotRequest.lamportClock);
        int visitorCapacity = boatSlotRequest.capacity;
        if(visitorCapacity <= capacityLeft)
        {
            capacityLeft -= visitorCapacity;
        }
        else
        {
            //check if boat has changed in meantime:
            pthread_mutex_lock(&waitForFreeBoatMutex);
            pthread_mutex_lock(&currentBoatMutex);            
            if(data->currentBoat == boardingBoat)
            {
                data->currentBoat = -2;
                pthread_mutex_unlock(&currentBoatMutex);
                //wait for next boat: new value of data->currentBoat, message: BOAT_SELECT
                pthread_cond_wait(&waitForFreeBoatCond, &waitForFreeBoatMutex); //wait for listening thread to receive BOAT_SELECT with next boat
                //update local value
                pthread_mutex_lock(&currentBoatMutex);            
                boardingBoat = data->currentBoat;
                pthread_mutex_unlock(&currentBoatMutex);
            }
            else
            {
                //boat has already changed, update local value
                boardingBoat = data->currentBoat;
                pthread_mutex_unlock(&currentBoatMutex);
                
            }
            pthread_mutex_lock(&boatsMutex);
            capacityLeft = data->boats[boardingBoat];
            pthread_mutex_unlock(&boatsMutex);
            pthread_mutex_unlock(&waitForFreeBoatMutex);
        }
    }
    //now visitor has the priority for place on boat
    if(data->visitorCapacity > capacityLeft)    //if no place for him, start trip and find next boat for boarding
    {
        pthread_mutex_lock(&boatRequestListMutex);
        captainId = (*data->boatRequestList[(data->boatRequestList).size()-1]).id; // last tourist who came into boat becomes a captain
        pthread_mutex_unlock(&boatRequestListMutex);
        printf("[%d]: Tourist [%d] became a CAPTAIN!\n", data->rank, captainId);              
        startTrip(data, data->currentBoat, data->boats[data->currentBoat], captainId);
        findFreeBoat(data, data->currentBoat);
    }
    clearBoatRequestList(data);

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
    prepareToRequest(data, message, WANNA_BOAT);

    //TODO: check if deadlock!!! if yes, then confirm may be needed instead...

    pthread_mutex_lock(&boatResponseMutex);
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
    // pthread_mutex_lock(&boatResponseMutex);
    data->necessaryBoatAnswers = data->size - 1;
    pthread_cond_wait(&boatResponseCond, &boatResponseMutex); //wait for signal from listening thread 
    pthread_mutex_unlock(&boatResponseMutex);
    //sort by lamport the list of candidates for boat
    sort((data->boatRequestList).begin(), (data->boatRequestList).end(), CompareBoatSlotRequest());

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
        printf("[%d]: I WANT A TRIP!\n", data->rank);

        //get pony suit
        findPony(data, message);
        
        //find place on boat and get on trip
        findPlaceOnBoat(data, message);
        //here the trip has just ended, but condition = ON_TRIP until pony is freed

        //free your pony suit - send permissions
        int ponyQueueSize = (data->ponyQueue).size();
        for (int i = 0; i < ponyQueueSize; i++)
        { // send message to all tourists waiting for a pony suit
            pthread_mutex_lock(&lamportMutex);
            data->lamportClock += 1;
            pthread_mutex_unlock(&lamportMutex);
            MPI_Send( message, sizeof(Packet), MPI_BYTE, (data->ponyQueue).front(), WANNA_PONY_RESPONSE, MPI_COMM_WORLD);
            (data->ponyQueue).pop();
            printf("[%d] -> [%d]: sent PONY permission\n", data->rank, i);
        }
        clearQueue(data->ponyQueue);

        pthread_mutex_lock(&conditionMutex);
        data->condition = IDLE;
        pthread_mutex_unlock(&conditionMutex);
        printf("[%d]: I'M IDLE!\n", data->rank);
    }
    delete message;
}

void mutexesInit()
{
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
    pthread_cond_init(&waitForDepartureCond, NULL);
    pthread_mutex_init(&waitForDepartureMutex, NULL); 
    pthread_cond_init(&waitForEndOfTripCond, NULL);
    pthread_mutex_init(&waitForEndOfTripMutex, NULL); 
    pthread_mutex_init(&boatRequestListMutex, NULL); 
    pthread_mutex_init(&boardedBoatCapacityMutex, NULL) ;
    pthread_mutex_init(&boardedBoatMutex, NULL); 
    pthread_mutex_init(&necessaryBoatAnswersMutex, NULL); 
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
