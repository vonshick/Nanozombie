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

//process statuses and message types (mpi tags)
#define IDLE -1
#define WANNA_PONY 10
#define WANNA_BOAT 20               //has pony, waiting for boat
//TODO: take ONBOARD into account when checking state
#define ONBOARD 30                  //onboard, ready for departure
#define ON_TRIP 40

//message types (mpi tags) only
#define INITIALIZATION 0
#define WANNA_PONY_RESPONSE 11      //response to 10       
#define WANNA_BOAT_RESPONSE 21      //response to 20, capacity > 0 means process wants a place on the boat (capacity == 0 - don't want a place)
#define CONFIRM_ONBOARD 31          //captain asks if passanger is onboard before he starts trip
#define CONFIRM_ONBOARD_RESPONSE 32 //passanger send response when he is onboard and ready for depart

//TODO: find and do todos :)
//TODO: boats and boatsmutex, how boat select depart and end of trip affect it? can it set boat as available when its not (i.e. late message)?
//TODO: make sure everyone who didnt get on boat waits until boat departs (what did I mean? xd)

//TODO: answers handling when on_board, on_trip?
#define END_OF_TRIP 50
#define BOAT_SELECT 100             //boat selected as next for boarding
#define BOAT_DEPART 110             //boat departed for trip
#define CURRENT_BOAT_ACK 120        //captain makes sure everyone has current boat set properly
#define CURRENT_BOAT_ACK_RESPONSE 121    //response to 120

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
sem_t confirmOnboardResponsesSem;
sem_t confirmOnboardRequestSem;
sem_t currentBoatAckResponsesSem;

//TODO: recheck mutexes after adding new case's in listening thread and modyfying functions in main thread
    //new case's: OK
pthread_mutex_t lamportMutex;               //CONFIRMED
pthread_mutex_t currentBoatMutex;           //CONFIRMED  //TODO: check if needed with respect to cond mutexes
pthread_mutex_t boatsMutex;                 //CONFIRMED  //check if needed with respect to cond mutexes and check if shoudl be associated with currentBoatMutex
pthread_mutex_t conditionMutex;             //CONFIRMED
pthread_mutex_t recentRequestClockMutex;    //CONFIRMED
pthread_mutex_t boardedBoatMutex;           //CONFIRMED
pthread_mutex_t findingFreeBoatMutex;       //CONFIRMED
pthread_mutex_t boatDeparturesQueueMutex;   //CONFIRMED

struct Packet
{
    Packet() {}
    Packet(int idArg, int cap, bool onTrip, int captId, int lampCl) : boatId(idArg), capacity(cap), captainId(captId), lamportClock(lampCl) { }
    int boatId;
    int capacity;       
    int captainId;      //process being captain on current trip
    int *passangers;    //list of passangers onboard departing boat
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

struct BoatDepartureMessage
{
    BoatDepartureMessage() {}
    BoatDepartureMessage(int boatIdArg, int captainIdArg, int capacityArg) :  boatId(boatIdArg), captainId(captainIdArg), capacity(capacityArg) {}
    int boatId;
    int captainId;
    int capacity;
};
 
struct Data
{
    bool run; 
    int condition;  //state of visitor: IDLE, WANNA_PONY, WANNA_BOAT, ON_TRIP
    int recentRequestClock;
    int necessaryPonyPermissions;
    int necessaryBoatResponses;
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
    int captainId;
    int confirmOnboardResponses;
    int currentBoatAckResponses;
};

//for sorting of boatRequestList vector
struct CompareBoatSlotRequest
{
    bool operator()(BoatSlotRequest * lhs, BoatSlotRequest * rhs)
    {
        if(lhs->lamportClock != rhs->lamportClock)
        {
            return lhs->lamportClock < rhs->lamportClock;
        }
        return lhs->id < rhs->id;
    }
};

void clearQueue(queue<int> &q)
{
   queue<int> empty;
   swap(q, empty);
}

int incrementLamport(Data *data)
{
    pthread_mutex_lock(&lamportMutex);
    data->lamportClock += 1;
    int printLamport = data->lamportClock;                   
    pthread_mutex_unlock(&lamportMutex);
    return printLamport;
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
    bool sendCurrentBoatAckResponse = false;
    
    while(data->run)
    {
        MPI_Recv(buffer, sizeof(Packet), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        pthread_mutex_lock(&lamportMutex);
        data->lamportClock = max(data->lamportClock, buffer->lamportClock) + 1;
        printLamport = data->lamportClock;
        pthread_mutex_unlock(&lamportMutex);

        switch(status.MPI_TAG) // check what kind of message came and call proper event
        {
            case WANNA_PONY:
                {    
                    printf("[%d]       : received WANNA PONY(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&conditionMutex);
                    if (data->condition == WANNA_BOAT || data->condition == ON_TRIP)
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        printf("[%d]       : added process [%d] WANNA PONY(%d) to PONY QUEUE\n", data->rank, status.MPI_SOURCE, buffer->lamportClock);                          
                        (data->ponyQueue).push(status.MPI_SOURCE);
                    }
                    else
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        if(data->condition == WANNA_PONY && (buffer->lamportClock > data->recentRequestClock || (buffer->lamportClock == data->recentRequestClock && status.MPI_SOURCE > data->rank)))
                        {
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            pthread_mutex_unlock(&conditionMutex);
                            printf("[%d]       : added process [%d] WANNA PONY(%d) to PONY QUEUE\n", data->rank, status.MPI_SOURCE, buffer->lamportClock);                          
                            (data->ponyQueue).push(status.MPI_SOURCE);   
                        }
                        else
                        {                
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            pthread_mutex_unlock(&conditionMutex);
                            response->lamportClock = buffer->lamportClock; //let know which request the response concerns (to handle old/unnecessary responses in requesting process)
                            printLamport = incrementLamport(data);
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
                        printf("[%d]       : received old PONY PERMISSION(%d) from [%d], skipping. (lamport: %d)\n", data->rank, response->lamportClock, status.MPI_SOURCE, printLamport);
                    }
                    else
                    {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        pthread_mutex_unlock(&conditionMutex);                        
                        data->necessaryPonyPermissions--;
                        if(data->necessaryPonyPermissions == 0)
                        {
                            printf("[%d]       : received last required PONY PERMISSION(%d) from [%d] (lamport: %d)\n", data->rank, response->lamportClock, status.MPI_SOURCE, printLamport);
                            sem_post(&ponyPermissionsSem);
                        }
                        else if(data->necessaryPonyPermissions > 0)
                        {
                            printf("[%d]       : received PONY PERMISSION(%d) from [%d]. Need %d more. (lamport: %d)\n", data->rank, response->lamportClock, status.MPI_SOURCE, data->necessaryPonyPermissions, printLamport);
                        }
                        else
                        {
                            printf("[%d]       : received unnecessary PONY PERMISSION(%d) from [%d]. (lamport: %d)\n",  data->rank, response->lamportClock, status.MPI_SOURCE, printLamport);
                        }
                    }
                }
                break;
            case WANNA_BOAT:
                {
                    printf("[%d]       : received WANNA BOAT(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
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
                    printLamport = incrementLamport(data);
                    MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    if(response->capacity == 0)
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I DONT WANT BOAT (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClock, printLamport);                                          
                    }
                    else
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I WANT BOAT(%d): my capacity = %d, (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClock, response->lamportClock, response->capacity, printLamport);                                          
                    }
                }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    pthread_mutex_lock(&recentRequestClockMutex);
                    int recentRequestClock = data->recentRequestClock;
                    pthread_mutex_unlock(&recentRequestClockMutex);
                    if(response->capacity == 0)
                    {
                        printf("[%d]       : received WANNA BOAT(%d) response I DON WANT BOAT from [%d] (lamport: %d)\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    }
                    else
                    {
                        printf("[%d]       : received WANNA BOAT(%d) response I WANT BOAT(%d) from [%d] (lamport: %d)\n", data->rank, recentRequestClock, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    }
                    if(buffer->capacity == 0) {
                        //if process don't want a place on boat - don't queue up his answer (decrease required number of answers in queue)
                        data->necessaryBoatResponses--;
                    }
                    else
                    {    //queue up the answer for place on boat
                        BoatSlotRequest *boatSlotRequest = new BoatSlotRequest(status.MPI_SOURCE, buffer->capacity, buffer->lamportClock);
                        (data->boatRequestList).push_back(boatSlotRequest);
                    }
                    if((data->boatRequestList).size() == data->necessaryBoatResponses)
                    {    //if all responses received
                        sem_post(&boatResponsesSem);
                    }
                }
                break;
            case BOAT_DEPART:
                {
                    //TODO check if you are onboard boat that departed without captain confiramtion
                    // should sem_post(confirmOnboardRequestSem) and set some flag?

                    printf("[%d]       : received BOAT DEPART(%d) from [%d] (lamport: %d)\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = 0;
                    pthread_mutex_unlock(&boatsMutex);
                    pthread_mutex_lock(&boardedBoatMutex);
                    if(data->boardedBoat == buffer->boatId)
                    {   //if i'm onboard
                        pthread_mutex_unlock(&boardedBoatMutex);
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
                    printf("[%d]       : received BOAT SELECT(%d) from [%d] (lamport: %d)\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&currentBoatMutex);
                    data->currentBoat = buffer->boatId;    //set id of selected boat as current boarding boat
                    pthread_mutex_unlock(&currentBoatMutex);
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = buffer->capacity;
                    pthread_mutex_unlock(&boatsMutex);
                    sem_post(&waitForBoatSelectSem);     //notify waiting visitor    
                    if(sendCurrentBoatAckResponse)
                    {
                        sendCurrentBoatAckResponse = false;
                        printLamport = incrementLamport(data);
                        MPI_Send(response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, CURRENT_BOAT_ACK_RESPONSE, MPI_COMM_WORLD);
                        printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);
                    }
                }
                break;
            case END_OF_TRIP:
                {
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = buffer->capacity; // add boat back to the free boats list
                    pthread_mutex_unlock(&boatsMutex);
                    pthread_mutex_lock(&boardedBoatMutex);
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == ON_TRIP && buffer->boatId == data->boardedBoat)
                    {   //if i'm onboard
                        printf("[%d]       : TRIP FINISHED! received END OF TRIP(%d) from [%d] (lamport: %d)\n", data->rank, buffer->lamportClock, status.MPI_SOURCE, printLamport);
                        pthread_mutex_unlock(&conditionMutex);
                        data->boardedBoat = -1;     // get off board
                        pthread_mutex_unlock(&boardedBoatMutex);
                        sem_post(&waitForEndOfTripSem);     //notify waiting visitor    
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        pthread_mutex_unlock(&boardedBoatMutex);
                        printf("[%d]       : received END OF TRIP from [%d]. NOT MY TRIP. (lamport: %d)\n", data->rank, status.MPI_SOURCE, printLamport);

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
            case CONFIRM_ONBOARD:
                printf("[%d]       : received CONFIRM ONBOARD request from captain [%d], boatId: %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);
                data->captainId = status.MPI_SOURCE;
                sem_post(&confirmOnboardRequestSem);
                break; 
            case CONFIRM_ONBOARD_RESPONSE:
                printf("[%d]       : received CONFIRM ONBOARD RESPONSE from [%d], he boarded at %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClock, printLamport);
                data->confirmOnboardResponses--;
                if(data->confirmOnboardResponses == 0)
                {
                    sem_post(&confirmOnboardResponsesSem);
                }
                break; 
            case CURRENT_BOAT_ACK:
                printf("[%d]       : received CURRENT BOAT ACK request from captain [%d], currentBoatId: %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);
                pthread_mutex_lock(&currentBoatMutex);            
                if(data->currentBoat == buffer->boatId)
                {
                    pthread_mutex_unlock(&currentBoatMutex);  
                    response->boatId = buffer->boatId;
                    printLamport = incrementLamport(data);
                    MPI_Send(response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, CURRENT_BOAT_ACK_RESPONSE, MPI_COMM_WORLD);
                    printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);                          
                }
                else
                {
                    pthread_mutex_unlock(&currentBoatMutex);            
                    sendCurrentBoatAckResponse = true;
                }
                break; 
            case CURRENT_BOAT_ACK_RESPONSE:
                printf("[%d]       : received CURRENT BOAT ACK RESPONSE from [%d]: currentBoat: %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);
                data->currentBoatAckResponses--;
                if(data->currentBoatAckResponses == 0)
                {
                    sem_post(&currentBoatAckResponsesSem);
                }
                break; 
            default:
                printf("[%d]       : WRONG MPI_TAG(%d) FROM [%d] (lamport: %d)\n", data->rank, status.MPI_TAG, status.MPI_SOURCE, printLamport);     
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


void clearLists(Data* data){
    //clear boatRequestList
    for(int i = 0;i < (data->boatRequestList).size();i++){
        delete (data->boatRequestList[i]);
    }
    data->boatRequestList.clear();
}

void sendBoatSelect(Data *data, int boatId)
{
    int selectTime = incrementLamport(data);
    Packet *message = new Packet;
    message->lamportClock = selectTime;
    message->boatId = boatId;
    pthread_mutex_lock(&boatsMutex);
    message->capacity = data->boats[data->currentBoat];
    pthread_mutex_unlock(&boatsMutex);

    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            int printLamport = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_SELECT, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: sent BOAT SELECT(%d) message: boatId: %d, capacity: %d (lamport: %d)\n", data->rank, i, selectTime, message->boatId, message->capacity, printLamport);
        }
    } 
    delete message;
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
        {   //found free boat
            pthread_mutex_unlock(&boatsMutex);
            pthread_mutex_lock(&currentBoatMutex);            
            data->currentBoat = i;
            pthread_mutex_unlock(&currentBoatMutex);
            pthread_mutex_unlock(&findingFreeBoatMutex);
            sendBoatSelect(data, i);
            return;
        }
    }
    //no free boats available now
    pthread_mutex_lock(&currentBoatMutex);
    data->currentBoat = -1; // -1 is status for visitor with priority for boarding
    pthread_mutex_unlock(&currentBoatMutex);
    pthread_mutex_unlock(&findingFreeBoatMutex);
    printf("[%d]       : !!! WAITING  !!! Didn't find free boat to select.\n", data->rank);
    sem_wait(&waitForBoatReturnSem); //wait for listening thread to set first boat that returned as currentBoat
    printf("[%d]       : !!! WOKEN UP !!! A boat has returned.\n", data->rank);
    //some boat has returned
    pthread_mutex_lock(&currentBoatMutex);
    int foundBoat = data->currentBoat;
    pthread_mutex_unlock(&currentBoatMutex);
    sendBoatSelect(data, foundBoat);
    return;
}

//passanger action
void waitForEndOfTrip(Data *data)
{
    int printLamport = setCondition(data, ON_TRIP);
    printf("[%d]       : IM ON TRIP! (lamport: %d)\n", data->rank, printLamport);
    sem_wait(&waitForEndOfTripSem); //wait for listening thread to receive END OF TRIP
}

//captains action
void manageTheTrip(Data* data)
{
    int printLamport = setCondition(data, ON_TRIP);
    printf("[%d]       : IM ON TRIP (AS CAPTAIN)! (lamport: %d)\n", data->rank, printLamport);

    Packet *message = new Packet;

    int waitMilisec = (rand() % (TRIP_MAX_DURATION-TRIP_MIN_DURATION)*1000) + TRIP_MIN_DURATION*1000;
    usleep(waitMilisec*1000); // captain decides how much time the trip takes

    message->lamportClock = incrementLamport(data);
    pthread_mutex_lock(&boardedBoatMutex);
    message->boatId = data->boardedBoat;
    data->boardedBoat = -1;   
    pthread_mutex_unlock(&boardedBoatMutex);
    message->capacity = data->boardedBoatCapacity;
    pthread_mutex_lock(&boatsMutex);
    data->boats[message->boatId] = data->boardedBoatCapacity; // add boat back to the list of free boats
    pthread_mutex_unlock(&boatsMutex);
    data->boardedBoatCapacity = 0;


    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            int printLamport = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, END_OF_TRIP, MPI_COMM_WORLD);    //send END OF TRIP message
            printf("[%d] -> [%d] sent END OF TRIP(%d) message (lamport: %d)\n", data->rank, i, message->lamportClock, printLamport);          
        }
    }
    delete message;
}

void startTrip(Data *data, int departingBoatId, int capacity, int captainId, int startTime, int *passangers)
{
    pthread_mutex_lock(&boatsMutex);
    data->boats[departingBoatId] = 0; //mark boat as unavailable (on trip)
    pthread_mutex_unlock(&boatsMutex);

    Packet *message = new Packet;
    message->captainId = captainId;
    message->boatId = departingBoatId;
    message->capacity = capacity;
    message->lamportClock = startTime;
    message->passangers = passangers;
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            int printLamport = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_DEPART, MPI_COMM_WORLD);    //send depart message
            printf("[%d] -> [%d]: sent BOAT DEPART(%d) message: departingBoatId: %d, captainId: %d, capacity: %d (lamport: %d)\n", data->rank, i, startTime, departingBoatId, captainId, capacity, printLamport);          
        }
    }    
    delete message;
}

void getOnBoard(Data *data, int boardingBoat, bool *passangers, int passangersCount, bool captain)
{
    data->boardedBoat = boardingBoat;
    int boardedTime = setCondition(data, ONBOARD);
    printf("[%d]       : I'M ONBOARD! (lamport: %d)\n", data->rank, boardedTime);

    Packet *message = new Packet;
    message->boatId = boardingBoat;
    int printLamport;
    data->confirmOnboardResponses = passangersCount;
    data->currentBoatAckResponses = data->size - passangersCount;
    if(captain)
    {   
        pthread_mutex_lock(&boatsMutex);
        data->boardedBoatCapacity = data->boats[boardingBoat];
        pthread_mutex_unlock(&boatsMutex);  
        for(int i = 0; i < data->size ;i++)
        {
            if(passangers[i])
            {   //check passangers are onboard
                printLamport = incrementLamport(data);
                MPI_Send(message, sizeof(Packet), MPI_BYTE, i, CONFIRM_ONBOARD, MPI_COMM_WORLD);
                printf("[%d] -> [%d]: captain sent CONFIRM ONBOARD request to passanger: departingBoatId: %d (lamport: %d)\n", data->rank, i, boardingBoat, printLamport);          
            }
            else
            {   //check non-passangers have proper current boarding boat
                printLamport = incrementLamport(data);
                MPI_Send(message, sizeof(Packet), MPI_BYTE, i, CURRENT_BOAT_ACK, MPI_COMM_WORLD);
                printf("[%d] -> [%d]: captain sent CURRENT BOAT ACK request to non-passanger: currentBoat: %d (lamport: %d)\n", data->rank, i, boardingBoat, printLamport);          
            }
        }
        //wait for passangers to be onboard
        sem_wait(&confirmOnboardResponsesSem);
        //wait for everyone to have proper current boat
        sem_wait(&currentBoatAckResponsesSem);

        //TODO: kolejnosc do potwierdzenia
        //
        //start trip
        //find free boat
        //manage trip
    }
    else
    {
        //wait for captain CONFIRM ONBOARD request or boat DEPART without previous CONFIRM ONBOARD request (see listening thread switch)
        sem_wait(&confirmOnboardRequestSem);
        if(data->captainId > -1)
        {   //respond to captain if DEPART hasn't arrived before CONFIRM ONBOARD
            message->lamportClock = boardedTime;
            printLamport = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, data->captainId, CONFIRM_ONBOARD_RESPONSE, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: passanger sent CONFIRM ONBOARD RESPONSE to captain: departingBoatId: %d (lamport: %d)\n", data->rank, data->captainId, boardingBoat, printLamport);          
            
            //TODO: check from this moment!
            sem_wait(&waitForDepartureSem); //wait for listening thread to receive BOAT DEPART
            //TODO: check this fucntion and update it
            waitForEndOfTrip(data);
        }
        else
        {   //DEPART received before CONFIRM ONBOARD  boat departed without me, find place again
            
        }
        
        //TODO: if captain didnt ask and boat departed, go back to finding place on next boat.
    }  
    delete message;
}

void clearPassangers(bool *passangers, int size)
{
    for (int i = 0; i < size; ++i) // Using for loop we are initializing
    {
        passangers[i] = false;
    }
}

void placeVisitorsInBoats(Data *data)
{
    pthread_mutex_lock(&currentBoatMutex);
    int boardingBoat = data->currentBoat;
    pthread_mutex_unlock(&currentBoatMutex);    
    pthread_mutex_lock(&boatsMutex);
    int capacityLeft = data->boats[boardingBoat];
    pthread_mutex_unlock(&boatsMutex);    

    bool passangers[data->size] = {false};  //true for visitor who will be on current boat
    int passangersCount = 0;
    int i = 0;
    BoatSlotRequest boatSlotRequest;
    bool satisfiedMyRequest = false;
    while(!satisfiedMyRequest) //runs until all visitors with higher priority are placed on boats and I found place as well - loop will break
    {
        boatSlotRequest = *(data->boatRequestList[i]);
        int visitorCapacity = boatSlotRequest.capacity;

        if(visitorCapacity <= capacityLeft)
        {
            capacityLeft -= visitorCapacity;
            i++;
            if(boatSlotRequest.id == data->rank)
            {   //found place for myself
                printf("[%d]       : FOUND SLOT FOR MYSELF! I'M on BOAT[%d] (%d element in boats queue: tourist [%d] with lamport %d)\n", data->rank, boardingBoat, i, boatSlotRequest.id, boatSlotRequest.lamportClock);
                satisfiedMyRequest = true;
            }
            else
            {   //found place for someone before me in queue
                passangers[boatSlotRequest.id] = true;
                printf("[%d]       : FOUND slot on BOAT[%d] for %d element in boats queue: tourist [%d] with lamport %d\n", data->rank, boardingBoat, i, boatSlotRequest.id, boatSlotRequest.lamportClock);
            }
        }
        else
        {
            printf("[%d]       : DIDN'T FIND slot on BOAT[%d] for %d element in boats queue: tourist [%d] with lamport %d\n", data->rank, boardingBoat, i, boatSlotRequest.id, boatSlotRequest.lamportClock);

            //wait for next boat: new value of data->currentBoat, message: BOAT SELECT
            printf("[%d]       : !!! WAITING  !!! For boarding boat[%d] change (BOAT SELECT).\n", data->rank, boardingBoat);
            sem_wait(&waitForBoatSelectSem); //wait for listening thread to receive BOAT SELECT with next boat
            printf("[%d]       : !!! WOKEN UP !!! Boarding boat[%d] has changed to [%d].\n", data->rank, boardingBoat, data->currentBoat);

            // boat has changed, update loop variables
            pthread_mutex_lock(&currentBoatMutex);            
            boardingBoat = data->currentBoat;
            pthread_mutex_unlock(&currentBoatMutex);
            pthread_mutex_lock(&boatsMutex);
            capacityLeft = data->boats[boardingBoat];
            pthread_mutex_unlock(&boatsMutex);
            clearPassangers(passangers, data->size);
            passangersCount = 0;
        }
    }
    bool captain = false;
    if(i < (data->boatRequestList).size())
    {   //if there is anybody behind me in list

        // TODO: ^^ what if nobody is behind when I asked, but then somebody wants to get on boat - he might think he will get onboard but i will satrt trip!
        //      is even it possible?
        boatSlotRequest = *(data->boatRequestList[i]);  //next request: first behind me
        //check if I'm captain: first behind me won't get place on this boat
        if(boatSlotRequest.capacity > capacityLeft)    //if no place for him, start trip and find next boat for boarding
        {
            captain = true;
        }
    }
    else
    {   //nobody behind me, I'm last who got on the boat
        captain = true;
    }
    
    getOnBoard(data, boardingBoat, passangers, passangersCount, captain);
    //trip has just ended
}

void findPlaceOnBoat(Data *data,  Packet *message)
{
    data->visitorCapacity = (rand() % data->maxVisitorCapacity) + 1;
    data->necessaryBoatResponses = data->size - 1;
    data->captainId = -1;
    
    clearLists(data);
    prepareToRequest(data, message, WANNA_BOAT);

    // send wanna boat request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            int printLamport = incrementLamport(data);
            MPI_Send( message, sizeof(Packet), MPI_BYTE, i, WANNA_BOAT, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent WANNA_BOAT(%d) request  (lamport: %d)\n", data->rank, i, message->lamportClock, printLamport);          
        }
    }
    //wait for all answers    
    sem_wait(&boatResponsesSem);
    //insert your own request
    pthread_mutex_lock(&recentRequestClockMutex);
    int recentRequestClock = data->recentRequestClock;
    pthread_mutex_unlock(&recentRequestClockMutex);
    (data->boatRequestList).push_back(new BoatSlotRequest(data->rank, data->visitorCapacity, recentRequestClock));

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

void findPony(Data *data, Packet *message)
{
    clearQueue(data->ponyQueue);
    
    int printLamport;
    data->necessaryPonyPermissions = data->size - data->numberOfPonies; //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
    prepareToRequest(data, message, WANNA_PONY);
    //send wanna pony request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            printLamport = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, WANNA_PONY, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent PONY(%d) request  (lamport: %d)\n", data->rank, i, message->lamportClock, printLamport);          
        }
    }    
    //wait for enough permissions
    printf("[%d]       : !!! WAITING  !!! for responses to WANNA_PONY(%d)) (lamport: %d)\n", data->rank, message->lamportClock, printLamport); 
    sem_wait(&ponyPermissionsSem);

    pthread_mutex_lock(&lamportMutex);
    printLamport = data->lamportClock;
    pthread_mutex_unlock(&lamportMutex);
    printf("[%d]       : !!! WOKEN UP !!! got PONY SUIT! (request WANNA_PONY(%d)) (lamport: %d)\n", data->rank, message->lamportClock, printLamport); 
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
        printf("[%d]       : I WANT A TRIP! (lamport: %d)\n", data->rank, printLamport);

        //get pony suit
        findPony(data, message);
        
        //find place on boat and get on trip
        findPlaceOnBoat(data, message);
        //here the trip has just ended, but condition = ON_TRIP until pony is freed

        //free your pony suit - send permissions

        printLamport = setCondition(data, IDLE);
        printf("[%d]       : I'M IDLE! Freeing pony: Sending queued permissions (lamport: %d)\n", data->rank, printLamport);

        for (int i = 0; i < (data->ponyQueue).size(); i++)
        { // send message to all tourists waiting for a pony suit
            printLamport = incrementLamport(data);
            MPI_Send( message, sizeof(Packet), MPI_BYTE, (data->ponyQueue).front(), WANNA_PONY_RESPONSE, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: sent queued PONY PERMISSION (lamport: %d)\n", data->rank, (data->ponyQueue).front(), printLamport);                          
            (data->ponyQueue).pop();
        }
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
    pthread_mutex_init(&boardedBoatMutex, NULL);
    pthread_mutex_init(&findingFreeBoatMutex, NULL);
    pthread_mutex_init(&boatDeparturesQueueMutex, NULL);
}

void semaphoresInit()
{
    sem_init(&ponyPermissionsSem, 0, 0);
    sem_init(&boatResponsesSem, 0, 0);
    sem_init(&waitForBoatReturnSem, 0, 0);
    sem_init(&waitForBoatSelectSem, 0, 0);
    sem_init(&waitForEndOfTripSem, 0, 0);
    sem_init(&waitForDepartureSem, 0, 0);
    sem_init(&confirmOnboardResponsesSem, 0, 0);
    sem_init(&confirmOnboardRequestSem, 0, 0);
    sem_init(&currentBoatAckResponsesSem, 0, 0);
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
