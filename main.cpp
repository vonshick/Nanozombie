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

//TODO: current boat ack? response yes only when idle or after finished processing and waiting depart
//todo no actions permitted between curr boat ack and boat depart

//process statuses and message types (mpi tags)
#define IDLE -1
#define WANNA_PONY 10
#define WANNA_BOAT 20               //has pony, waiting for boat

//not needed, after succesful boarding they go idle and wait rand period of time
// #define ONBOARD 30                  //onboard, ready for departure
// #define ON_TRIP 40

//message types (mpi tags) only
#define INITIALIZATION 0
#define WANNA_PONY_RESPONSE 11      //response to 10       
#define WANNA_BOAT_RESPONSE 21      //response to 20, capacity > 0 means process wants a place on the boat (capacity == 0 - don't want a place)
#define CONFIRM_ONBOARD 31          //captain asks if passanger is onboard before he starts trip
#define CONFIRM_ONBOARD_RESPONSE 32 //passanger send response when he is onboard and ready for depart

#define BOAT_SELECT 100             //boat selected as next for boarding
#define BOAT_DEPART 110             //boat departed for trip
#define PASSANGERS 111              //passangers depaarted in 110

using namespace std;

//seconds:
const int VISITOR_MAX_WAIT = 10;    //tenths of sec
const int VISITOR_MIN_WAIT = 5;    //tenths of sec

sem_t ponyPermissionsSem;
sem_t boatResponsesSem;
sem_t boatDepartSem;

pthread_mutex_t lamportMutex;               //CONFIRMED
pthread_mutex_t currentBoatMutex;           //CONFIRMED
pthread_mutex_t conditionMutex;             //CONFIRMED
pthread_mutex_t recentRequestClockMutex;    //CONFIRMED

struct Packet
{
    Packet() {}
    Packet(int idArg, int nextB, int lampCl, int lampClkReq, bool pax) : boatId(idArg), nextBoat(nextB), lamportClock(lampCl), lamportClockOfRequest(lampClkReq), passanger(pax) { }
    int boatId;
    int nextBoat;
    int lamportClock;
    int lamportClockOfRequest;
    bool passanger;
};

struct BoatSlotRequest
{
    BoatSlotRequest() {}
    BoatSlotRequest(int idArg, int lamportClockArg) :  id(idArg), lamportClock(lamportClockArg) {}
    int id;
    int lamportClock;
};

struct PonyRequest
{
    PonyRequest(int idArg,int lamportClockOfRequestArg) :  id(idArg), lamportClockOfRequest(lamportClockOfRequestArg) {}
    int id;
    int lamportClockOfRequest;
};
 
struct Data
{
    bool run; 
    // bool firstTrip;
    int boatDepartsCounter;
    int condition;  //state of visitor: IDLE, WANNA_PONY, WANNA_BOAT, ON_TRIP
    int recentRequestClock;
    int necessaryPonyPermissions;
    int necessaryBoatResponses;
    int *boats;     //capacity of each boat
    queue<int> boatsReadyQueue;
    //todo mutex?
    int currentBoat;    //id of current boarding boat
    int numberOfPonies;
    int numberOfBoats;
    int maxBoatCapacity;
    int maxVisitorCapacity;
    int *visitorsCapacity;  //capacity of visitors
    queue<PonyRequest*> ponyQueue;
    vector<BoatSlotRequest*> boatRequestList; //list of requests for place on boat
    int rank; 
    int size; 
    int lamportClock;
    int respondTo;
    int confirmBoatId;
    bool failedBoardng;
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

int getLamport(Data *data)
{
    pthread_mutex_lock(&lamportMutex);
    int printLamport = data->lamportClock;                   
    pthread_mutex_unlock(&lamportMutex);
    return printLamport;
}

int getFreeBoat(Data *data)
{
    int nextBoat = (data->boatsReadyQueue).front();
    (data->boatsReadyQueue).pop();
    (data->boatsReadyQueue).push(nextBoat);
    data->currentBoat = nextBoat;
    printf("[%d]       : set next boarding boat to %d (lamport: %d).\n", data->rank, nextBoat, getLamport(data));
    return nextBoat;
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

        switch(status.MPI_TAG) // check what kind of message came and call proper event
        {
            case WANNA_PONY:
                {    
                    printf("[%d]       : received WANNA PONY(%d) from [%d] (lamport: %d).\n", data->rank, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                    pthread_mutex_lock(&conditionMutex);
                    if (data->condition == WANNA_BOAT)
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
                    if(data->condition == WANNA_BOAT)
                    {
                        pthread_mutex_lock(&recentRequestClockMutex);
                        response->lamportClockOfRequest = data->recentRequestClock;
                        pthread_mutex_unlock(&recentRequestClockMutex);     
                        pthread_mutex_unlock(&conditionMutex);
                    }
                    else
                    {
                        pthread_mutex_unlock(&conditionMutex);
                        response->lamportClockOfRequest = -10;
                    }
                    response->lamportClock = incrementLamport(data);
                    MPI_Send( response, sizeof(Packet), MPI_BYTE, status.MPI_SOURCE, WANNA_BOAT_RESPONSE, MPI_COMM_WORLD);
                    if(response->lamportClockOfRequest == -10)
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I DONT WANT BOAT (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest, response->lamportClock);                                          
                    }
                    else
                    {
                        printf("[%d] -> [%d]: sent WANNA BOAT(%d) response I WANT BOAT(%d): (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->lamportClockOfRequest, response->lamportClockOfRequest, response->lamportClock);                                          
                    }
                }
                break;
            case WANNA_BOAT_RESPONSE:
                {
                    pthread_mutex_lock(&recentRequestClockMutex);
                    int recentRequestClock = data->recentRequestClock;
                    pthread_mutex_unlock(&recentRequestClockMutex);
                    if(buffer->lamportClockOfRequest != -10)
                    {   //queue up the answer for place on boat
                        printf("[%d]       : received WANNA BOAT(%d) response I WANT BOAT(%d) from [%d] (lamport: %d)\n", data->rank, recentRequestClock, buffer->lamportClockOfRequest, status.MPI_SOURCE, printLamport);
                        BoatSlotRequest *boatSlotRequest = new BoatSlotRequest(status.MPI_SOURCE, buffer->lamportClockOfRequest);
                        (data->boatRequestList).push_back(boatSlotRequest);
                    }
                    else
                    {   //if process don't want a place on boat - don't queue up his answer (decrease required number of answers in queue)
                        printf("[%d]       : received WANNA BOAT(%d) response I DON'T WANT BOAT from [%d] (lamport: %d)\n", data->rank, recentRequestClock, status.MPI_SOURCE, printLamport);
                        data->necessaryBoatResponses--;
                    }
                    if((data->boatRequestList).size() == data->necessaryBoatResponses)
                    {    //if all responses received
                        sem_post(&boatResponsesSem);
                    }
                }
                break;
            case BOAT_DEPART:
                {
                    //clear everything, new round
                    //todo send only when he already wants boat
                    // if(data->firstTrip)
                    // {
                    //     data->firstTrip = false;
                    // }

                    // todo is it needed at all?
                    // mutex needed 
                    data->boatDepartsCounter++;
                    //possible verify: cpt sends his counter, compare it with mine
                    // if(nextBoat != buffer->nextBoat)
                    // {
                    //     printf("[%d]       : ERROR received nextBoat %d but my is %d (lamport: %d)\n", data->rank, buffer->nextBoat, nextBoat, printLamport);     
                    // }
                    printf("[%d]       : received BOAT_DEPART from [%d], boatID: %d (lamport: %d)\n", data->rank, status.MPI_SOURCE, buffer->boatId, printLamport);
                    pthread_mutex_lock(&currentBoatMutex);
                    int nextBoat = getFreeBoat(data);
                    pthread_mutex_unlock(&currentBoatMutex);
                    if(nextBoat != buffer->nextBoat)
                    {
                        printf("[%d]       : ERROR received nextBoat %d but my is %d (lamport: %d)\n", data->rank, buffer->nextBoat, nextBoat, printLamport);     
                    }
                    sem_post(&boatDepartSem);
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

void printPassangers(Data *data, bool *passangers)
{
    printf("[%d]       : I'M CAPTAIN! STARTING TRIP (lamport: %d) PAXES:\n", data->rank, getLamport(data));
    for (int i = 0; i < data->size; ++i)
    {
        if(passangers[i] && i != data->rank)
        {
            printf("%d, ", i);
        }
    }
    printf("\n");
}

void startTrip(Data *data, bool *passangers, int currentBoat)
{
    Packet *message = new Packet;
    message->boatId = currentBoat;
    pthread_mutex_lock(&currentBoatMutex);
    message->nextBoat = getFreeBoat(data);
    pthread_mutex_unlock(&currentBoatMutex);   
    printPassangers(data, passangers);
    for(int i = 0; i < data->size ;i++)
    {
        if(i != data->rank)
        {
            if(passangers[i])
            {
                message->passanger = true;
            }
            else
            {
                message->passanger = false;
            }
            message->lamportClock = incrementLamport(data);
            MPI_Send(message, sizeof(Packet), MPI_BYTE, i, BOAT_DEPART, MPI_COMM_WORLD);    //send depart message
            printf("[%d] -> [%d]: sent BOAT DEPART message: departingBoatId: %d, (lamport: %d)\n", data->rank, i, currentBoat, message->lamportClock);          
            //TODO if sync on mutex after each depart, must send debart to himself      
        }
    }  
    delete message;
}

bool boatFull(Data *data, int spaceLeft, bool *passangers)
{
    for(int i = 0; i < data->size; i++) {
        if(data->visitorsCapacity[i] <= spaceLeft)
        {   //there is still enough space for him
            if(!passangers[i])
            {   //he is not already on the boat
                return false;
            }
        }
    }

    //TODO!!! he may not receive pony!!
    //FREE PONY WHEN DIDNT GET ON BOAT! :D
    //still there may be not enough ponies xd
    //make parameters for that

    //no space for anyone else
    return true;
}

bool placeVisitorsInBoats(Data *data, int currentBoat)
{
    int size = (data->boatRequestList).size();    
    int boatCapacity = data->boats[currentBoat];
    bool *passangers = new bool[data->size];
    BoatSlotRequest *request;
    for(int j = 0; j < data->size; j++)
    {
        passangers[j] = false;
    }
    bool foundPlace = false;
    int i = 0;
    while(i < size)
    {   
        request = data->boatRequestList[i];
        boatCapacity -= data->visitorsCapacity[request->id];
        if(boatCapacity < 0)
        {   //no more room on boat
            if(request->id == data->rank)
            {
                printf("[%d]       : DIDN'T FIND SLOT! FREEING RESOURCES AND WAITING FOR NEXT BOAT (lamport: %d)\n", data->rank, getLamport(data));
                // sem_wait(&boatDepartSem);
                break;
            }
            else
            {   //this one didnt get on boat, maybe next will
                boatCapacity += data->visitorsCapacity[request->id];
                printf("[%d]       : no place for %d, he needs %d but %d left\n", data->rank, request->id, data->visitorsCapacity[request->id], boatCapacity);
            }
        }
        else
        {
            passangers[request->id] = true;
            if(request->id == data->rank)
            {   //got place
                if(boatFull(data, boatCapacity, passangers))
                {
                    startTrip(data, passangers, currentBoat);
                }
                else
                {
                    printf("[%d]       : I'M ONBOARD! WAITING DEPART (lamport: %d)\n", data->rank, getLamport(data));
                    sem_wait(&boatDepartSem);
                    printf("[%d]       : SUCCESS! I'M ON TRIP!\n", data->rank);
                }
                foundPlace = true;
                break;
            }
            else
            {
                printf("[%d]       : tourist %d onboard, he took %d and left %d for others\n", data->rank, request->id, data->visitorsCapacity[request->id], boatCapacity);
            }
        }
        i++;
    }
    delete[] passangers;
    return foundPlace;
}

bool findPlaceOnBoat(Data *data,  Packet *message)
{

    //todo do we need this? if someone new wants boat, can he receive old answer, from previous boarding? or join new boarding thinking its still old one?
    //do we have to synchronize joining? maybe in boat request we should validate boarding counter?
    // if(!(data->firstTrip))
    // {
    //     sem_wait(&boatDepartSem);
    // }

    //todo it must be recent depart, not old one
    // make sure no possible answer no, then go yes in one boarding

    data->necessaryBoatResponses = data->size - 1;
    printf("[%d]       : clearBoatRequestLists\n", data->rank);
    clearBoatRequestLists(data);
    pthread_mutex_lock(&currentBoatMutex);
    int currentBoat = data->currentBoat;
    pthread_mutex_unlock(&currentBoatMutex);
    int printLamport = prepareToRequest(data, message, WANNA_BOAT);

    printf("[%d]       : I WANT BOAT! boarding: %d (lamport: %d)\n", data->rank, currentBoat, printLamport);

    // after every depart everyone reports wanna boat again

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
    (data->boatRequestList).push_back(new BoatSlotRequest(data->rank, recentRequestClock));

    //sort by lamport the list of candidates for boat
    sort((data->boatRequestList).begin(), (data->boatRequestList).end(), CompareBoatSlotRequest());   
    //print sorted     
    printf("\n[%d]: CURRENT LIST for boarding boat %d (space: %d): (lamport %d)\n", data->rank, data->currentBoat, data->boats[data->currentBoat], getLamport(data));          
    for (vector<BoatSlotRequest*>::const_iterator i = (data->boatRequestList).begin(); i != (data->boatRequestList).end(); ++i)
    {
        printf("lamport: [%d] id: [%d] capacity: [%d]\n", (*i)->lamportClock, (*i)->id, data->visitorsCapacity[(*i)->id]);          
    }
    printf("==LIST END==\n\n"); 

    //find your boat by placing other visitors in boats with regard to lamport
    bool foundPlace = placeVisitorsInBoats(data, currentBoat);
    return foundPlace;
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
        int waitMilisec = (rand() % (VISITOR_MAX_WAIT-VISITOR_MIN_WAIT)*100) + VISITOR_MIN_WAIT*100;
        usleep(waitMilisec*1000);
        printf("[%d]       : I WANT A TRIP!\n", data->rank);

        //get pony suit
        findPony(data, message);
        
        //find place on boat and get on trip
        //todo priority for next when didnt get on trip
        bool foundPlace = findPlaceOnBoat(data, message);

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

        if(!foundPlace)
        {
            sem_wait(&boatDepartSem);
        }
    }
    delete message;
}

void mutexesInit()
{
    pthread_mutex_init(&lamportMutex, NULL);
    pthread_mutex_init(&currentBoatMutex, NULL); 
    pthread_mutex_init(&conditionMutex, NULL);
    pthread_mutex_init(&recentRequestClockMutex, NULL); 
}

void semaphoresInit()
{
    sem_init(&ponyPermissionsSem, 0, 0);
    sem_init(&boatResponsesSem, 0, 0);
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
    // MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    mutexesInit();
    semaphoresInit();

    queue<PonyRequest*> ponyQueue;  //queue for pony, storing process id
    queue<int> boatsReadyQueue;  //queue for ready boats waiting in bay for trip
    vector<BoatSlotRequest*> boatRequestList;  //queue for boat place requests

    Data *data = new Data;
    data->run = true;
    // data->firstTrip = true;
    data->boatDepartsCounter = 0;
    data->condition = IDLE;    
    data->lamportClock = 0;
    data->recentRequestClock = 0;
    data->rank = rank; 
    data->size = size;
    data->ponyQueue = ponyQueue;
    data->boatsReadyQueue = boatsReadyQueue;
    data->boatRequestList = boatRequestList;
    data->numberOfPonies = atoi(argv[1]);
    data->numberOfBoats = atoi(argv[2]);
    data->maxBoatCapacity = atoi(argv[3]);
    data->maxVisitorCapacity = atoi(argv[4]);
    data->boats = new int[data->numberOfBoats];
    data->visitorsCapacity = new int[size];
    data->confirmBoatId = 0;
    data->currentBoat = 0;
    data->failedBoardng = false;
    
    srand(time(NULL));      
    if (rank == 0)
    {
        check_thread_support(provided);
        int max = data->maxBoatCapacity;
        int min = data->maxVisitorCapacity;
        for(int i = 0; i < data->numberOfBoats; i++)
        {
            if(i != 0) (data->boatsReadyQueue).push(i);
            data->boats[i] = (rand() % (max - min + 1)) + min;  //boat capacity in range [maxVisitorCapacity ; maxBoatCapacity]
            printf("Boat[%d] capacity: %d\n", i, data->boats[i]);
        }
        //passangers' sizes
        for(int i = 0; i < size; i++)
        {
            data->visitorsCapacity[i] = (rand() % data->maxVisitorCapacity) + 1;
            printf("visitorsCapacity[%d] capacity: %d\n", i, data->visitorsCapacity[i]);
        } 
        //share data with reamining threads
        for(int i = 1; i < size; i++)
        {
            MPI_Send(data->boats, data->numberOfBoats, MPI_INT, i, INITIALIZATION, MPI_COMM_WORLD); //send array with boats capacity
            MPI_Send(data->visitorsCapacity, data->size, MPI_INT, i, INITIALIZATION, MPI_COMM_WORLD); //send array with visitors capacity
        }        
    }
    else
    {
        MPI_Recv(data->boats, data->numberOfBoats, MPI_INT, 0, INITIALIZATION, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //wait for boats capacity
        MPI_Recv(data->visitorsCapacity, data->size, MPI_INT, 0, INITIALIZATION, MPI_COMM_WORLD, MPI_STATUS_IGNORE); //wait for visitors capacity
        for(int i = 1; i < data->numberOfBoats; i++)
        {
            (data->boatsReadyQueue).push(i);
        }
    }

    //create listening thread - receiving messages
    pthread_t listeningThread;
    if (pthread_create(&listeningThread, NULL, listen, (void *) data))
    {
        MPI_Finalize();
        exit(0);
    }   

    //initializatin completed, starting main logic
    visit(data);

    MPI_Finalize();
    delete[] data->boats;
    delete[] data->visitorsCapacity;
    delete data;

    return 0;
}
