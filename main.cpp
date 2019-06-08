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

//TODO: two captains!

//process statuses and message types (mpi tags)
#define IDLE -1
#define WANNA_PONY 10
#define WANNA_BOAT 20               //has pony, waiting for boat
#define ONBOARD 30                  //onboard, ready for departure
#define ON_TRIP 40

//message types (mpi tags) only
#define INITIALIZATION 0
#define WANNA_PONY_RESPONSE 11      //response to 10       
#define WANNA_BOAT_RESPONSE 21      //response to 20, capacity > 0 means process wants a place on the boat (capacity == 0 - don't want a place)
#define CONFIRM_ONBOARD 31          //captain asks if passanger is onboard before he starts trip
#define CONFIRM_ONBOARD_RESPONSE 32 //passanger send response when he is onboard and ready for depart

#define END_OF_TRIP 50
#define BOAT_SELECT 100             //boat selected as next for boarding
#define BOAT_DEPART 110             //boat departed for trip
#define PASSANGERS 111              //passangers depaarted in 110
#define CONFIRM_CURRENT_BOAT 120        //captain makes sure everyone has current boat set properly
#define CONFIRM_CURRENT_BOAT_RESPONSE 121    //response to 120

using namespace std;

//seconds:
const int VISITOR_MAX_WAIT = 2;
const int VISITOR_MIN_WAIT = 1;
const int TRIP_MIN_DURATION = 1;
const int TRIP_MAX_DURATION = 2;

sem_t ponyPermissionsSem;
sem_t boatResponsesSem;
sem_t waitForBoatReturnSem;
// sem_t waitForBoatSelectSem;
sem_t waitForEndOfTripSem;
sem_t confirmOnboardResponsesSem;
sem_t confirmStateToCaptainSem;
sem_t currentBoatAckResponsesSem;
sem_t boatDepartSem;

pthread_mutex_t lamportMutex;               //CONFIRMED
pthread_mutex_t currentBoatMutex;           //CONFIRMED
pthread_mutex_t boatsMutex;                 //PROBABLY NOT NEEDED
pthread_mutex_t conditionMutex;             //CONFIRMED
pthread_mutex_t recentRequestClockMutex;    //CONFIRMED
pthread_mutex_t boardedBoatMutex;           //CONFIRMED
pthread_mutex_t findingFreeBoatMutex;       //CONFIRMED
pthread_mutex_t boatDeparturesQueueMutex;   //CONFIRMED
// pthread_mutex_t holdBoatSelectMutex;        //CONFIRMED
pthread_mutex_t holdBoatDepartMutex;        //CONFIRMED

struct Packet
{
    Packet() {}
    Packet(int idArg, int cap, bool onTrip, int captId, int lampCl) : boatId(idArg), capacity(cap), captainId(captId), lamportClock(lampCl) { }
    int boatId;
    int nextBoatId;
    int capacity;       
    int captainId;      //process being captain on current trip
    int lamportClock;
    int lamportClockOfRequest;
};

struct BoatSlotRequest
{
    BoatSlotRequest() {}
    BoatSlotRequest(int idArg, int capacityArg, int lamportClockArg) :  id(idArg), capacity(capacityArg), lamportClock(lamportClockArg) {}
    int id;
    int capacity;
    int lamportClock;
};

struct PonyRequest
{
    PonyRequest(int idArg,int lamportClockOfRequestArg) :  id(idArg), lamportClockOfRequest(lamportClockOfRequestArg) {}
    int id;
    int lamportClockOfRequest;
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
    queue<PonyRequest*> ponyQueue;
    vector<BoatSlotRequest*> boatRequestList; //list of requests for place on boat
    int rank; 
    int size; 
    int lamportClock;
    int captainId;
    int confirmOnboardResponses;
    int currentBoatAckResponses;
    int *passangersDeparted;
    int *lastWannaBoatKnowledge;  //lamport clocks of most recent WANNA BOAT request of all processes
    int recentlyDepartedBoat;
    int nextBoatId;
    int nextBoatCapacity;
    int respondTo;
    int confirmBoatId;
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
    int sendConfirmCurrentBoatResponse = -1;
    
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
                    printf("[%d]       : received WANNA PONY(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&conditionMutex);
                    if (data->condition == WANNA_BOAT || data->condition == ONBOARD || data->condition == ON_TRIP)
                    {
                        printf("[%d]       : added process [%d] WANNA PONY(%d) to PONY QUEUE\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest);                          
                        (data->ponyQueue).push(new PonyRequest(status.MPI_SOURCE, buffer->lamportClockOfRequest));
                        pthread_mutex_unlock(&conditionMutex);
                    }
                    else
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        if(data->condition == WANNA_PONY && (buffer->lamportClockOfRequest > data->recentRequestClock || (buffer->lamportClockOfRequest == data->recentRequestClock && status.MPI_SOURCE > data->rank)))
                        {
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            printf("[%d]       : added process [%d] WANNA PONY(%d) to PONY QUEUE\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest);                          
                            (data->ponyQueue).push(new PonyRequest(status.MPI_SOURCE, buffer->lamportClockOfRequest));   
                            pthread_mutex_unlock(&conditionMutex);
                        }
                        else
                        {                
                            pthread_mutex_unlock(&recentRequestClockMutex);
                            response->lamportClockOfRequest = buffer->lamportClockOfRequest; //let know which request the response concerns (to handle old/unnecessary responses in requesting process)
                            response->lamportClock = incrementLamport(data);
                            MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_PONY_RESPONSE, MPI_COMM_WORLD); // send message about pony suit request 
                            printf("[%d] -> [%d]: sent PONY PERMISSION(%d) (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest, response->lamportClock);                          
                            pthread_mutex_unlock(&conditionMutex);
                        }
                    }
                }
                break;
            case WANNA_PONY_RESPONSE:
                {
                    pthread_mutex_lock(&conditionMutex);
                    pthread_mutex_lock(&recentRequestClockMutex);                    
                    if(data->condition != WANNA_PONY || buffer->lamportClockOfRequest != data->recentRequestClock)
                    {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        pthread_mutex_unlock(&conditionMutex);                        
                        printf("[%d]       : received old PONY PERMISSION(%d) from [%d], skipping. (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                    }
                    else
                    {
                        pthread_mutex_unlock(&recentRequestClockMutex);
                        pthread_mutex_unlock(&conditionMutex);                        
                        data->necessaryPonyPermissions--;
                        if(data->necessaryPonyPermissions == 0)
                        {
                            printf("[%d]       : received last required PONY PERMISSION(%d) from [%d] (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                            sem_post(&ponyPermissionsSem);
                        }
                        else if(data->necessaryPonyPermissions > 0)
                        {
                            printf("[%d]       : received PONY PERMISSION(%d) from [%d]. Need %d more. (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, data->necessaryPonyPermissions, printLamport);
                        }
                        else
                        {
                            printf("[%d]       : received unnecessary PONY PERMISSION(%d) from [%d]. (lamport: %d)\n",  data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                        }
                    }
                }
                break;
            case WANNA_BOAT:
                {
                    printf("[%d]       : received WANNA BOAT(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == WANNA_BOAT || data->condition == ONBOARD)
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        pthread_mutex_lock(&lamportMutex);
                        response->lamportClockOfRequest = data->recentRequestClock;
                        data->lastWannaBoatKnowledge[status.MPI_SOURCE] = data->lamportClock;
                        pthread_mutex_unlock(&lamportMutex);
                        pthread_mutex_unlock(&recentRequestClockMutex);     
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = data->visitorCapacity;
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        response->capacity = 0;
                    }
                    response->lamportClock = incrementLamport(data);
                    MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    //he knows for sure if my request lamport is smallerMPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    if(response->capacity == 0)
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I DONT WANT BOAT (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest, response->lamportClock);                                          
                    }
                    else
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I WANT BOAT(%d): my capacity = %d, (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest, response->lamportClockOfRequest, response->capacity, response->lamportClock);                                          
                    }
                }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    pthread_mutex_lock(&recentRequestClockMutex);
                    int recentRequestClock = data->recentRequestClock;
                    pthread_mutex_unlock(&recentRequestClockMutex);
                    if(buffer->capacity == 0)
                    {   //if process don't want a place on boat - don't queue up his answer (decrease required number of answers in queue)
                        printf("[%d]       : received WANNA BOAT(%d) response I DON'T WANT BOAT from [%d] (lamport: %d)\n", data->rank, recentRequestClock, status.MPI_SOURCE, printLamport);
                        data->necessaryBoatResponses--;
                    }
                    else
                    {   //queue up the answer for place on boat
                        printf("[%d]       : received WANNA BOAT(%d) response I WANT BOAT(%d) from [%d] (lamport: %d)\n", data->rank, recentRequestClock, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                        BoatSlotRequest *boatSlotRequest = new BoatSlotRequest(status.MPI_SOURCE, buffer->capacity, buffer->lamportClockOfRequest);
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
                    printf("[%d]       : received BOAT DEPART(%d) from [%d], departingBoat: %d, nextBoat: %d (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, buffer->boatId, buffer->nextBoatId, printLamport);
                    
                    pthread_mutex_lock(&holdBoatDepartMutex);
                    
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = 0;
                    pthread_mutex_unlock(&boatsMutex);
                    int paxLocal[data->size];
                    MPI_Recv(paxLocal, data->size, MPI_INT, status.MPI_SOURCE, PASSANGERS, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    printf("[%d]       : received PASSANGERS to BOAT DEPART(%d) from [%d]  (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                    
                    if(sendConfirmCurrentBoatResponse > -1)
                    {
                        response->lamportClock = incrementLamport(data);
                        MPI_Send(response, sizeof(Packet), MPI_BYTE, sendConfirmCurrentBoatResponse, CONFIRM_CURRENT_BOAT_RESPONSE, MPI_COMM_WORLD);
                        printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, response->lamportClock);
                        sendConfirmCurrentBoatResponse = -1;
                    }
                    
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == WANNA_BOAT)
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        for(int i=0; i < data->size; i++)
                        {
                            (data->passangersDeparted)[i] = paxLocal[i];
                        }
                        data->recentlyDepartedBoat = buffer->boatId;
                        data->nextBoatId = buffer->nextBoatId;
                        data->nextBoatCapacity = buffer->capacity;
                        sem_post(&boatDepartSem);
                    }
                    else
                    {
                        pthread_mutex_lock(&currentBoatMutex);
                        data->currentBoat = buffer->nextBoatId;    //set id of selected boat as current boarding boat
                        pthread_mutex_unlock(&currentBoatMutex);
                        pthread_mutex_lock(&boatsMutex);
                        //TODO: check nextBoatId is everywhere where needed
                        data->boats[buffer->nextBoatId] = buffer->capacity;
                        pthread_mutex_unlock(&boatsMutex);
                        pthread_mutex_unlock(&conditionMutex);
                    }
                    pthread_mutex_unlock(&holdBoatDepartMutex);
                }
                break;
            // case BOAT_SELECT:
            //     {
            //         printf("[%d]       : received BOAT SELECT(%d) from [%d] (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
            //         pthread_mutex_lock(&holdBoatSelectMutex);
            //         pthread_mutex_lock(&currentBoatMutex);
            //         data->currentBoat = buffer->boatId;    //set id of selected boat as current boarding boat
            //         pthread_mutex_unlock(&currentBoatMutex);
            //         pthread_mutex_lock(&boatsMutex);
            //         data->boats[buffer->boatId] = buffer->capacity;
            //         pthread_mutex_unlock(&boatsMutex);
            //         pthread_mutex_unlock(&holdBoatSelectMutex);
                    
            //         //Moved to boat depart
            //         // sem_post(&waitForBoatSelectSem);     //notify waiting visitor 
            //         // if(sendConfirmCurrentBoatResponse > -1)
            //         // {
            //         //     response->lamportClock = incrementLamport(data);
            //         //     MPI_Send(response, sizeof(Packet), MPI_BYTE, sendConfirmCurrentBoatResponse, CONFIRM_CURRENT_BOAT_RESPONSE, MPI_COMM_WORLD);
            //         //     printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, response->lamportClock);
            //         //     sendConfirmCurrentBoatResponse = -1;
            //         // }
            //     }
            //     break;
            case END_OF_TRIP:
                {
                    pthread_mutex_lock(&boatsMutex);
                    data->boats[buffer->boatId] = buffer->capacity; // add boat back to the free boats list
                    pthread_mutex_unlock(&boatsMutex);
                    pthread_mutex_lock(&boardedBoatMutex);
                    pthread_mutex_lock(&conditionMutex);
                    if(data->condition == ON_TRIP && buffer->boatId == data->boardedBoat)
                    {   //if i'm onboard
                        printf("[%d]       : TRIP FINISHED! received END OF TRIP(%d) from [%d] (lamport: %d)\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
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
                        if(data->currentBoat == -1) //if I'm captain waiting for any boat to select as next
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
                sem_post(&confirmStateToCaptainSem);
                break; 
            case CONFIRM_ONBOARD_RESPONSE:
                printf("[%d]       : received CONFIRM ONBOARD RESPONSE from [%d], he boarded at %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest, printLamport);
                data->confirmOnboardResponses--;
                if(data->confirmOnboardResponses == 0)
                {
                    sem_post(&confirmOnboardResponsesSem);
                }
                break; 
            case CONFIRM_CURRENT_BOAT:
                printf("[%d]       : received CURRENT BOAT ACK request from captain [%d], currentBoatId: %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);
                pthread_mutex_lock(&conditionMutex);
                if(data->condition == WANNA_BOAT)
                {
                    pthread_mutex_unlock(&conditionMutex);
                    data->respondTo = status.MPI_SOURCE;
                    data->confirmBoatId = buffer->boatId;
                    sem_post(&confirmStateToCaptainSem);
                }
                else
                {
                    pthread_mutex_unlock(&conditionMutex);
                    //TODO: check if its ok
                    pthread_mutex_lock(&currentBoatMutex);      
                    if(data->currentBoat == buffer->boatId)
                    {
                        pthread_mutex_unlock(&currentBoatMutex);  
                        response->boatId = buffer->boatId;
                        response->lamportClock = incrementLamport(data);
                        MPI_Send(response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, CONFIRM_CURRENT_BOAT_RESPONSE, MPI_COMM_WORLD);
                        printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, response->lamportClock);                          
                    }
                    else
                    {
                        pthread_mutex_unlock(&currentBoatMutex);            
                        sendConfirmCurrentBoatResponse = status.MPI_SOURCE;
                    }
                }
                
                break; 
            case CONFIRM_CURRENT_BOAT_RESPONSE:
                data->currentBoatAckResponses--;
                printf("[%d]       : received CURRENT BOAT ACK RESPONSE from [%d]. Need %d more. currentBoat: %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, data->currentBoatAckResponses, buffer->boatId, printLamport);
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
    int lamport;
    pthread_mutex_lock(&conditionMutex);
    pthread_mutex_lock(&lamportMutex);
    data->condition = status;
    data->lamportClock += 1;
    lamport = data->lamportClock;
    pthread_mutex_unlock(&lamportMutex); 
    pthread_mutex_unlock(&conditionMutex);
    return lamport;
}

int prepareToRequest(Data *data, Packet *message, int newCondition)
{
    int lamport;
    pthread_mutex_lock(&conditionMutex);
    pthread_mutex_lock(&recentRequestClockMutex);
    pthread_mutex_lock(&lamportMutex);
    data->condition = newCondition;
    data->lamportClock += 1; // increment lamportClock before sending request
    lamport = data->lamportClock;
    data->recentRequestClock = data->lamportClock;
    message->lamportClockOfRequest = data->recentRequestClock; // set request lamport timestamp in packet 
    pthread_mutex_unlock(&lamportMutex);
    pthread_mutex_unlock(&recentRequestClockMutex);
    pthread_mutex_unlock(&conditionMutex);
    return lamport;
}

void clearBoatRequestLists(Data *data)
{
    while((data->boatRequestList).size() > 0)
    {   
        int i = ((data->boatRequestList).back())->id;
        delete (data->boatRequestList).back();
        (data->boatRequestList).pop_back();
        printf("[%d]:    deleted request of process [%d]\n", data->rank, i);
    }
}

void printPassangers(int *passangers, int size, int myId)
{
    printf("[%d]       : Starting trip! Paxes: \n", myId);
    for (int i = 0; i < size; ++i) // Using for loop we are initializing
    {
        if(passangers[i] == 1 && i != myId)
        {
            printf("%d, ", i);
        }
    }
    printf("\n");
}

// void sendBoatSelect(Data *data, int boatId)
// {
//     int selectTime = incrementLamport(data);
//     Packet *message = new Packet;
//     message->lamportClockOfRequest = selectTime;
//     message->boatId = boatId;
//     pthread_mutex_lock(&boatsMutex);
//     message->capacity = data->boats[data->currentBoat];
//     pthread_mutex_unlock(&boatsMutex);

//     for(int i = 0; i < data->size ;i++)
//     {
//         if(i != data->rank)
//         {
//             message->lamportClock = incrementLamport(data);
//             MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_SELECT, MPI_COMM_WORLD);
//             printf("[%d] -> [%d]: sent BOAT SELECT(%d) message: boatId: %d, capacity: %d (lamport: %d)\n", data->rank, i, selectTime, message->boatId, message->capacity, message->lamportClock);
//         }
//     } 
//     delete message;
// }

int findFreeBoat(Data *data)
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
            // sendBoatSelect(data, i);
            return i;
        }
    }
    //no free boats available now
    pthread_mutex_lock(&currentBoatMutex);
    data->currentBoat = -1; // -1 is status for captain
    pthread_mutex_unlock(&currentBoatMutex);
    pthread_mutex_unlock(&findingFreeBoatMutex);
    printf("[%d]       : !!! WAITING  !!! Didn't find free boat to select.\n", data->rank);
    sem_wait(&waitForBoatReturnSem); //wait for listening thread to set first boat that returned as currentBoat
    printf("[%d]       : !!! WOKEN UP !!! A boat has returned.\n", data->rank);
    //some boat has returned
    pthread_mutex_lock(&currentBoatMutex);
    int foundBoat = data->currentBoat;
    pthread_mutex_unlock(&currentBoatMutex);
    // sendBoatSelect(data, foundBoat);
    return foundBoat;
}

//captains action
void manageTheTrip(Data* data)
{
    Packet *message = new Packet;

    int waitMilisec = (rand() % (TRIP_MAX_DURATION-TRIP_MIN_DURATION)*1000) + TRIP_MIN_DURATION*1000;
    usleep(waitMilisec*1000); // captain decides how much time the trip takes

    message->lamportClockOfRequest = incrementLamport(data);
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
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, END_OF_TRIP, MPI_COMM_WORLD);    //send END OF TRIP message
            printf("[%d] -> [%d] sent END OF TRIP(%d) message (lamport: %d)\n", data->rank, i, message->lamportClock, message->lamportClock);          
        }
    }
    delete message;
}

void startTrip(Data *data, int departingBoatId, int captainId, int startTime, int *passangers)
{
    pthread_mutex_lock(&boatsMutex);
    data->boats[departingBoatId] = 0; //mark boat as unavailable (on trip)
    pthread_mutex_unlock(&boatsMutex);

    Packet *message = new Packet;
    message->captainId = captainId;
    message->boatId = departingBoatId;
    message->lamportClockOfRequest = startTime;
    // message->passangers = new bool[data->size];
    // for(int i=0; i < data->size; i++)
    // {
    //     (message->passangers)[i] = passangers[i];
    // }
    message->nextBoatId = findFreeBoat(data);
    pthread_mutex_lock(&boatsMutex);
    message->capacity = data->boats[message->nextBoatId];
    pthread_mutex_unlock(&boatsMutex);
    printPassangers(passangers, data->size, data->rank);
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_DEPART, MPI_COMM_WORLD);    //send depart message
            printf("[%d] -> [%d]: sent BOAT DEPART(%d) message: departingBoatId: %d, captainId: %d, nextBoat: %d (lamport: %d)\n", data->rank, i, startTime, departingBoatId, captainId, message->nextBoatId, message->lamportClock);          
            MPI_Send(passangers, data->size, MPI_INT, i, PASSANGERS, MPI_COMM_WORLD);    //send passangers as additional info 
            printf("[%d] -> [%d]: sent PASSANGERS message with BOAT DEPART(%d)\n", data->rank, i, departingBoatId);          
        }
    }  

    int printLamport = setCondition(data, ON_TRIP);
    printf("[%d]       : IM ON TRIP (AS CAPTAIN)! (lamport: %d)\n", data->rank, printLamport);  
    // delete[] message->passangers;
    delete message;
}

bool getOnBoard(Data *data, int boardingBoat, int *passangers, int passangersCount, bool captain)
{
    data->boardedBoat = boardingBoat;
    int boardedTime = setCondition(data, ONBOARD);
    pthread_mutex_unlock(&holdBoatDepartMutex);
    printf("[%d]       : I'M ONBOARD! (lamport: %d)\n", data->rank, boardedTime);

    Packet *message = new Packet;
    message->boatId = boardingBoat;
    if(captain)
    {   
        data->confirmOnboardResponses = passangersCount;
        data->currentBoatAckResponses = data->size - passangersCount - 1;
        pthread_mutex_lock(&boatsMutex);
        data->boardedBoatCapacity = data->boats[boardingBoat];
        pthread_mutex_unlock(&boatsMutex);  
        for(int i = 0; i < data->size ;i++)
        {
            if(i != data->rank)
            {
                if(passangers[i] == 1)
                {   //check passangers are onboard
                    message->lamportClock = incrementLamport(data);
                    MPI_Send(message, sizeof(Packet), MPI_BYTE, i, CONFIRM_ONBOARD, MPI_COMM_WORLD);
                    printf("[%d] -> [%d]: captain sent CONFIRM ONBOARD request to passanger: departingBoatId: %d (lamport: %d)\n", data->rank, i, boardingBoat, message->lamportClock);          
                }
                else
                {   //check non-passangers have proper current boarding boat
                    message->lamportClock = incrementLamport(data);
                    MPI_Send(message, sizeof(Packet), MPI_BYTE, i, CONFIRM_CURRENT_BOAT, MPI_COMM_WORLD);
                    printf("[%d] -> [%d]: captain sent CURRENT BOAT ACK request to non-passanger: currentBoat: %d (lamport: %d)\n", data->rank, i, boardingBoat, message->lamportClock);          
                }
            }
        }
        //wait for passangers to be onboard
        if(data->confirmOnboardResponses > 0)
        {
            sem_wait(&confirmOnboardResponsesSem);
        }
        //wait for everyone to have proper current boat
        if(data->currentBoatAckResponses > 0)
        {
            sem_wait(&currentBoatAckResponsesSem);
        }
        
        startTrip(data, boardingBoat, data->rank, incrementLamport(data), passangers);
        // findFreeBoat(data, boardingBoat);
        manageTheTrip(data);
        //trip has ended
        delete message;
        return true;
    }
    else
    {
        //wait for captain CONFIRM ONBOARD request or boat DEPART without previous CONFIRM ONBOARD request (see listening thread switch)
        // printf("[%d]       : wait for holdBoatSelectMutex\n", data->rank);
        // pthread_mutex_lock(&holdBoatSelectMutex);
        printf("[%d]       : Passanger: wait for confirmStateToCaptainSem\n", data->rank);
        sem_wait(&confirmStateToCaptainSem);
        if(data->captainId > -1)
        {   //respond to captain if everything is ok and waiting for DEPART to arrive after just received CONFIRM ONBOARD
            int printLamport = setCondition(data, ON_TRIP);
            // pthread_mutex_unlock(&holdBoatSelectMutex);
            printf("[%d]       : IM GOING ON TRIP! (lamport: %d)\n", data->rank, printLamport);
            message->lamportClockOfRequest = boardedTime;
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, data->captainId, CONFIRM_ONBOARD_RESPONSE, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: passanger sent CONFIRM ONBOARD RESPONSE to captain: departingBoatId: %d (lamport: %d)\n", data->rank, data->captainId, boardingBoat, message->lamportClock);          
            sem_wait(&boatDepartSem);   //wait for depart
            sem_wait(&waitForEndOfTripSem); //wait for listening thread to receive END OF TRIP
            //trip has ended
            delete message;
            return true;
        }
        else
        {   //received CONFIRM_CURRENT_BOAT instead of expected CONFIRM_ONBOARD: boat departed without me, find place again
            if(boardingBoat == data->confirmBoatId)
            {
                message->lamportClock = incrementLamport(data);
                message->boatId = boardingBoat;
                MPI_Send(message, sizeof(Packet), MPI_BYTE, data->respondTo, CONFIRM_CURRENT_BOAT_RESPONSE, MPI_COMM_WORLD);
                printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, data->respondTo, boardingBoat, message->lamportClock);                          
            }
            else
            {
                printf("[%d] ERRROR2 : WRONG CURRENT BOAT ID! I AM BOARDING %d, CAPTAIN ASKS FOR %d\n", data->rank, boardingBoat, data->confirmBoatId);
            }
            //wait for depart
            sem_wait(&boatDepartSem);
            delete message;
            return false;
        }
    }  
}

//delete from list passangers who got on trip
void updateBoatSlotRequestList(Data *data)
{
    vector<BoatSlotRequest*> newBoatRequestList;  //new queue for boat place requests
    BoatSlotRequest *boatSlotRequest;
    
    for(int i = 0; i < (data->boatRequestList).size(); i++)
    {
        boatSlotRequest = data->boatRequestList[i];
        printf("[%d]:    checking list request %d\n", data->rank, i);
        if((data->passangersDeparted)[boatSlotRequest->id] == 1)
        {
            printf("[%d]:    found departed passanger %d\n", data->rank, boatSlotRequest->id);
            (data->passangersDeparted)[boatSlotRequest->id] = 0;
            delete boatSlotRequest;
            printf("[%d]:    deleted departed passanger\n", data->rank);
        }
        else
        {
            printf("[%d]:    passanger %d hasnt departed, stays in list \n", data->rank, boatSlotRequest->id);
            newBoatRequestList.push_back(boatSlotRequest);
        }
        
    }
    data->boatRequestList = newBoatRequestList;
}

void placeVisitorsInBoats(Data *data)
{
    Packet *message = new Packet;
    bool gotOnTrip = false;
    while(!gotOnTrip)
    {
        printf("[%d]       : wait for holdBoatDepartMutex\n", data->rank);
        pthread_mutex_lock(&holdBoatDepartMutex);

        //update current boat
        if(data->recentlyDepartedBoat == data->currentBoat)
        {
            printf("[%d]:    updating list\n", data->rank);
            updateBoatSlotRequestList(data);
            pthread_mutex_lock(&lamportMutex);
            data->currentBoat = data->nextBoatId;   //set id of selected boat as current boarding boat
            pthread_mutex_lock(&boatsMutex);    //TODO: needed mutrex??
            data->boats[data->nextBoatId] = data->nextBoatCapacity;
            pthread_mutex_unlock(&boatsMutex);
        }
        printf("\n[%d]: CURRENT LIST for boarding boat %d: (lamport %d)\n", data->rank, data->currentBoat, data->lamportClock);          
        pthread_mutex_unlock(&lamportMutex);
        for (vector<BoatSlotRequest*>::const_iterator i = (data->boatRequestList).begin(); i != (data->boatRequestList).end(); ++i)
        printf("lamport: [%d] id: [%d] capacity: [%d]\n", (*i)->lamportClock, (*i)->id, (*i)->capacity);          
        printf("==LIST END==\n\n");  

        // pthread_mutex_lock(&currentBoatMutex);
        int boardingBoat = data->currentBoat;
        // pthread_mutex_unlock(&currentBoatMutex);    
        pthread_mutex_lock(&boatsMutex);
        int capacityLeft = data->boats[boardingBoat];
        pthread_mutex_unlock(&boatsMutex);    

        int passangers[data->size] = {0};  //true for visitor who will be on current boat
        int passangersCount = 0;
        BoatSlotRequest boatSlotRequest;
        bool imOnBoard = false;
        bool boatIsFull = false;
        bool willGetOnTrip = true;
        int i = 0;
        while(!imOnBoard && !boatIsFull) //boarding a boat - runs until I'm on boat or boat is full
        {
            boatSlotRequest = *(data->boatRequestList[i]);
            int visitorCapacity = boatSlotRequest.capacity;

            if(visitorCapacity <= capacityLeft)
            {   //if there is enough space for next visitor
                capacityLeft -= visitorCapacity;
                passangers[boatSlotRequest.id] = 1;
                i++;
                if(boatSlotRequest.id == data->rank)
                {   //found place for myself
                    printf("[%d]       : FOUND SLOT FOR MYSELF! I'M on BOAT[%d] (%d element in boats queue: tourist [%d] with lamport %d)\n", data->rank, boardingBoat, i-1, boatSlotRequest.id, boatSlotRequest.lamportClock);
                    imOnBoard = true;
                }
                else
                {   //found place for someone before me in queue
                    passangersCount++; //here so captain won't be included
                    printf("[%d]       : FOUND slot on BOAT[%d] for %d element in boats queue: tourist [%d] with lamport %d\n", data->rank, boardingBoat, i-1, boatSlotRequest.id, boatSlotRequest.lamportClock);
                    //check if he knows about my request
                    //TODO: was >, will >= be ok?
                    if(data->recentRequestClock >= (data->lastWannaBoatKnowledge)[boatSlotRequest.id])
                    {   //he doesn't know so he will be captain and depart without me
                        printf("[%d]       : I won't get on board! my request: %d, lastWannaBoatKnowledge of [%d]: %d\n", data->rank, data->recentRequestClock, boatSlotRequest.id, (data->lastWannaBoatKnowledge)[boatSlotRequest.id]);
                        willGetOnTrip = false;
                    }
                    else
                    {
                        printf("[%d]       : I will get on board! my request: %d, lastWannaBoatKnowledge of [%d]: %d\n", data->rank, data->recentRequestClock, boatSlotRequest.id, (data->lastWannaBoatKnowledge)[boatSlotRequest.id]);
                    }
                }
            }
            else
            {   //if boat is full
                printf("[%d]       : BOAT FULL: DIDN'T FIND slot on BOAT[%d] for %d element in boats queue: tourist [%d] with lamport %d\n", data->rank, boardingBoat, i-1, boatSlotRequest.id, boatSlotRequest.lamportClock);
                boatIsFull = true;
            }
        }
        if(imOnBoard && willGetOnTrip)  // if found a place, get on board. Else, boat will depart without me, so continue looking for place in next boat
        {  
            int whichIf = 0;
            bool captain = false;
            if(i < (data->boatRequestList).size())
            {   //if there is somebody behind me in list
                whichIf = 1;
                boatSlotRequest = *(data->boatRequestList)[i];  //next request: first behind me
                //check if I'm captain: first behind me won't get place on this boat
                if(boatSlotRequest.capacity > capacityLeft)    //if no place for him, start trip and find next boat for boarding
                {
                    whichIf = 2;
                    captain = true;
                }
            }
            else
            {   //nobody behind me, I'm last who got on the boat
                whichIf = 3;
                captain = true;
            }

            printf("[%d]       : going onboard as captain: %d (%d).\n", data->rank, captain, whichIf);
            gotOnTrip = getOnBoard(data, boardingBoat, passangers, passangersCount, captain);
            if(!gotOnTrip)
            {   //if false, didn't manage to get on this trip, will try in next boat (already received boat depart)
                printf("[%d]       : TRIP FAIL: found slot on BOAT[%d] but they departed without me (I was late) lamport %d\n\nHANDLING NEEDS TO BE CHECKED\n\n\n", data->rank, boardingBoat, incrementLamport(data));
            }
        }
        else
        {   // wait for next boat
            pthread_mutex_unlock(&holdBoatDepartMutex);

            //wait for boat ack
            printf("[%d]       : Non-passanger: wait for confirmStateToCaptainSem\n", data->rank);
            sem_wait(&confirmStateToCaptainSem);
            //send reply - need rank to response: data->respondTo

            if(boardingBoat == data->confirmBoatId)
            {
                message->lamportClock = incrementLamport(data);
                message->boatId = boardingBoat;
                MPI_Send(message, sizeof(Packet), MPI_BYTE, data->respondTo, CONFIRM_CURRENT_BOAT_RESPONSE, MPI_COMM_WORLD);
                printf("[%d] -> [%d]: sent CURRENT BOAT ACK RESPONSE, currentBoatId: %d  (lamport: %d)\n", data->rank, data->respondTo, boardingBoat, message->lamportClock);                          
            }
            else
            {
                printf("[%d] ERRROR1 : WRONG CURRENT BOAT ID! I AM BOARDING %d, CAPTAIN ASKS FOR %d\n", data->rank, boardingBoat, data->confirmBoatId);
            }

            //wait for depart
            sem_wait(&boatDepartSem);
        }
    }
    delete message;
}

void findPlaceOnBoat(Data *data,  Packet *message)
{
    data->visitorCapacity = (rand() % data->maxVisitorCapacity) + 1;
    data->necessaryBoatResponses = data->size - 1;
    data->captainId = -1;
    printf("[%d]       : clearBoatRequestLists\n", data->rank);
    clearBoatRequestLists(data);
    data->recentlyDepartedBoat = -1;
    data->nextBoatId = -1;
    int printLamport = prepareToRequest(data, message, WANNA_BOAT);

    //TODO: dent boat ack response, then I want boat, then not onboard, so waiting for ack resp... but already received!
    //solution: hold for boat select always after boat ack?

    printf("[%d]       : I WANT BOAT! (lamport: %d)\n", data->rank, printLamport);
    // send wanna boat request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, WANNA_BOAT, MPI_COMM_WORLD); //send message about boat request 
            printf("[%d] -> [%d]: sent WANNA_BOAT(%d) request  (lamport: %d)\n", data->rank, i, message->lamportClockOfRequest, message->lamportClock);          
        }
    }
    
    //wait for all answers    
    sem_wait(&boatResponsesSem);
    //insert your own request
    pthread_mutex_lock(&recentRequestClockMutex);
    int recentRequestClock = data->recentRequestClock;
    pthread_mutex_unlock(&recentRequestClockMutex);
    (data->boatRequestList).push_back(new BoatSlotRequest(data->rank, data->visitorCapacity, recentRequestClock));

    //sort by lamport the list of candidates for boat
    sort((data->boatRequestList).begin(), (data->boatRequestList).end(), CompareBoatSlotRequest());        

    //find your boat by placing other visitors in boats with regard to lamport
    placeVisitorsInBoats(data);
}

void findPony(Data *data, Packet *message)
{
    data->necessaryPonyPermissions = data->size - data->numberOfPonies; //if we got (numberOfTourists - numberOfPonies) answers that suit is free we can be sure that's true and take it
    int printLamport = prepareToRequest(data, message, WANNA_PONY);
    printf("[%d]       : I WANT PONY SUIT! (lamport: %d)\n", data->rank, printLamport);
    //send wanna pony request
    for(int i = 0; i < data->size; i++)
    {
        if(i != data->rank)
        {
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, WANNA_PONY, MPI_COMM_WORLD); //send message about pony suit request 
            printf("[%d] -> [%d]: sent PONY(%d) request  (lamport: %d)\n", data->rank, i, message->lamportClockOfRequest, message->lamportClock);          
        }
    }    
    //wait for enough permissions
    printf("[%d]       : !!! WAITING  !!! for responses to WANNA_PONY(%d))\n", data->rank, message->lamportClockOfRequest); 
    if(data->necessaryPonyPermissions > 0)
    {
        sem_wait(&ponyPermissionsSem);
    }
    printf("[%d]       : !!! WOKEN UP !!! got PONY SUIT! (request WANNA_PONY(%d))\n", data->rank, message->lamportClockOfRequest); 
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
        printf("[%d]       : I WANT A TRIP!\n", data->rank);

        //get pony suit
        findPony(data, message);
        
        //find place on boat and get on trip
        findPlaceOnBoat(data, message);
        //here the trip has just ended

        int printLamport = setCondition(data, IDLE);
        printf("[%d]       : I'M IDLE! Freeing pony: Sending queued permissions (lamport: %d)\n", data->rank, printLamport);
        //free your pony suit - send permissions
        while((data->ponyQueue).size() > 0)
        {   // send message to all tourists waiting for a pony suit
            message->lamportClockOfRequest = ((data->ponyQueue).front())->lamportClockOfRequest;
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, ((data->ponyQueue).front())->id, WANNA_PONY_RESPONSE, MPI_COMM_WORLD);
            printf("[%d] -> [%d]: sent queued PONY PERMISSION(%d) (lamport: %d)\n", data->rank, ((data->ponyQueue).front())->id, message->lamportClockOfRequest, message->lamportClock);                          
            delete (data->ponyQueue).front();
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
    // pthread_mutex_init(&holdBoatSelectMutex, NULL);
    pthread_mutex_init(&holdBoatDepartMutex, NULL);
}

void semaphoresInit()
{
    sem_init(&ponyPermissionsSem, 0, 0);
    sem_init(&boatResponsesSem, 0, 0);
    sem_init(&waitForBoatReturnSem, 0, 0);
    // sem_init(&waitForBoatSelectSem, 0, 0);
    sem_init(&waitForEndOfTripSem, 0, 0);
    sem_init(&confirmOnboardResponsesSem, 0, 0);
    sem_init(&confirmStateToCaptainSem, 0, 0);
    sem_init(&currentBoatAckResponsesSem, 0, 0);
    sem_init(&boatDepartSem, 0, 0);
}

void check_thread_support(int provided) {
    printf("THREAD SUPPORT: %d\n", provided);
    switch (provided)
    {
    case MPI_THREAD_SINGLE:
        printf("Brak wsparcia dla wątków, kończę\n");
        fprintf(stderr, "Brak wystarczającego wsparcia dla wątków - wychodzę!\n");
        MPI_Finalize();
        exit(-1);
        break;
    case MPI_THREAD_FUNNELED:
        printf("Tylko te wątki, ktore wykonaly mpi_init_thread mogą wykonać wołania do biblioteki mpi\n");
        break;
    case MPI_THREAD_SERIALIZED:
        /* Potrzebne zamki wokół wywołań biblioteki MPI*/
        printf("Tylko jeden watek naraz może wykonać wołania do biblioteki MPI\n");
        break;
    case MPI_THREAD_MULTIPLE:
        printf("Pełne wsparcie dla wątków\n");
        break;
    default:
        printf("Nikt nic nie wie\n");
    }
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
    if(atoi(argv[2]) < 2)
    {
	    cout << "We need at least 2 boats!\n";
	    exit(0);
    }
    int rank, size, provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    check_thread_support(provided);
    // MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    mutexesInit();
    semaphoresInit();

    queue<PonyRequest*> ponyQueue;  //queue for pony, storing process id
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
    data->lastWannaBoatKnowledge = new int[size];
    data->confirmBoatId = 0;
    data->passangersDeparted = new int[size];
    for(int i=0; i < data->size; i++)
    {
        (data->passangersDeparted)[i] = 0;
        (data->lastWannaBoatKnowledge)[i] = 0;
    }
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
    delete[] data->passangersDeparted;
    delete[] data->lastWannaBoatKnowledge;
    delete data;

    return 0;
}
