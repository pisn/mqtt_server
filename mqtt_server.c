#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define LISTENQ 1
#define MAXDATASIZE 100
#define MAXMESSAGESHISTORY 500
#define MAXLINE 65000
#define MAXTOPICS 200
#define MAXSUBSCRIPTIONS 200

int verbose;

typedef struct {
    char* ClientIdentification;
    //botar tempo do keepAlive depois
    char** interestedTopics;
    uint interestedTopicsLength;
    uint currentHead;
    int connfd;
    int stopSignal;
} activeConnection;

typedef struct {
    int remainingLength, multiplierOffset;
} remainingLengthStruct;

/******************* Received messages storage ******************************/

typedef struct {
    uint messageLength;
    uint topicLength;
    
    u_int8_t* message;
    char* topicName;
} receivedMessage;

typedef struct {
    receivedMessage** history;
    uint head;
    uint totalCount;
} receivedMessagesCircularList;

pthread_mutex_t storageLock;

receivedMessagesCircularList* createMessagesStorage(){    
    pthread_mutex_init(&storageLock, NULL);
    
    receivedMessagesCircularList* list = malloc(sizeof(receivedMessagesCircularList));
    list->history = malloc(MAXMESSAGESHISTORY*sizeof(receivedMessage*));
    list->head=0;

    return list;
}

void addMessageToList(receivedMessagesCircularList* list, receivedMessage* message){    
    pthread_mutex_lock(&storageLock);
    
    list->head = (list->head + 1)%MAXMESSAGESHISTORY; 
    (list->totalCount)++;   

    if(list->head < list->totalCount){
        if(verbose){
            printf("Freeing old message from structure\n");
        }
        free(list->history[list->head]);
    }

    list->history[list->head] = message;      

    pthread_mutex_unlock(&storageLock);
}

receivedMessage *getReceivedMessage(receivedMessagesCircularList *list, uint index){
    return list->history[index];
}


void destroyMessageStorage(receivedMessagesCircularList *list){
    int positionsToBeFreed = MAXMESSAGESHISTORY;

    if(list->totalCount < positionsToBeFreed){
        positionsToBeFreed = list->totalCount;
    }

    for(int i=0;i<positionsToBeFreed;i++){
        free((list->history[i])->message);
        free((list->history[i])->topicName);        
    }        
    free(list->history);    
    free(list);
    
    pthread_mutex_destroy(&storageLock);
}

receivedMessagesCircularList* messagesStorage;

/******************************************************************************/

/*****************Common functions for messages processing ********************/

remainingLengthStruct* decodeRemainingLength(uint8_t* remainingLengthStream) {
    int multiplier = 1;
    int value = 0;
    int offset = -1;
    uint8_t encodedByte;

    do{
        offset++;
        encodedByte = remainingLengthStream[offset];
        value += (encodedByte & 127) * multiplier;

        if (multiplier > 128*128*128){
            printf("Malformed Remaining Length\n");
            exit(8);
        }               

        multiplier *= 128;        

    } while ((encodedByte & 128) != 0);

    remainingLengthStruct *returnLength = malloc(sizeof(remainingLengthStruct));
    returnLength->multiplierOffset = offset;
    returnLength->remainingLength = value;

    return returnLength;    
}

typedef struct {
    uint16_t stringLength;
    char* string;
} utf8String;

utf8String* decodeUtf8String(uint8_t* utf8EncodedStream){
    uint16_t stringLength = (utf8EncodedStream[0] << 8) | utf8EncodedStream[1];
    
    if(verbose)
        printf("UTF8 string Size:%d\n", stringLength);

    utf8String* returnString = malloc(sizeof(utf8String));
    returnString->stringLength = stringLength;
    returnString->string = malloc(stringLength*sizeof(char));

    for(uint16_t i=0; i<stringLength;i++){
        returnString->string[i] = utf8EncodedStream[2+i];
    }

    return returnString;
}

/************************************************************************/

/**********************MESSAGES FOR SENDING PACKETS **********************/

void CONNACK(uint8_t returnCode, int connfd){
    uint8_t responseStream[4]; //CONNACK tem tamanho fixo, nao tem payload

    responseStream[0] = 32;
    responseStream[1] = 2;
    responseStream[2] = 0; //Por enquanto nao estou tratando SessionPresent.
    responseStream[3] = returnCode;

    if(verbose)
        printf("Writing CONNACK %d\n", returnCode);

    write(connfd, responseStream, 4);
}

void SUBACK(uint16_t packetIdentifier, uint8_t returnCodes[MAXTOPICS], uint returnCodeLength, int connfd){
    uint8_t *responseStream = malloc((returnCodeLength + 4)*sizeof(uint8_t));

    responseStream[0] = 144;
    responseStream[1] = 2 + returnCodeLength;
    responseStream[2] = packetIdentifier >> 8;
    responseStream[3] = packetIdentifier & 15;    
    
    for(uint i=0; i<returnCodeLength; i++){
        responseStream[4 + i] = returnCodes[i];
    }

    if(verbose)
        printf("Writing SUBACK\n");

    write(connfd, responseStream, returnCodeLength + 4);

    free(responseStream);    
}

void PINGRESP(int connfd){
    uint8_t responseStream[2]; //CONNACK tem tamanho fixo, nao tem payload

    responseStream[0] = 208;
    responseStream[1] = 0;    
    
    write(connfd, responseStream, 2);
}

/**************************************************************************/

/*********************RECEIVED MESSAGES FUNCTIONS *************************/

void CONNECT(activeConnection *connection, uint8_t flags, uint8_t* receivedCommunication, int remainingLength, int connfd){
    int offset = 0;

    if(flags != 0){        
        printf("Invalid flags! Close network connection. [MQTT-2.2.2-2]");
        close(connfd);
        return;
    }

    uint16_t protocolNameLength = (receivedCommunication[0] << 8) | receivedCommunication[1];
    offset +=2;

    char* protocolName = malloc(protocolNameLength);

    for(uint16_t i=0; i<protocolNameLength; i++){
        protocolName[i] = receivedCommunication[offset+i];
    }
    offset += protocolNameLength;

    if(verbose)
        printf("Protocol Name:%s\n", protocolName);

    if(strcmp(protocolName,"MQTT") != 0){
        printf("Unrecognized protocol. Closing connection. [MQTT-3.1.2-1]\n");
        close(connfd); //protocolo nao reconhecido
        return;
    }

    uint8_t protocolLevel = receivedCommunication[offset];
    offset++;

    if(protocolLevel != 4){
        printf("Unrecognized protocol level %d. [MQTT-3.1.2-2]\n", protocolLevel);
        CONNACK(1, connfd);
        close(connfd);
        return;
    }

    uint8_t connectionFlags = receivedCommunication[offset];
    offset++;

    if(connectionFlags & 1){
        printf("Control reserved bit set. ClosingConnection.  [MQTT-3.1.2-3]\n");
        close(connfd);
        return;
    }

    //OBS: como nao devo me preocupar com falhas agora, nao vou me preocupar com o Cleanssession Flag. Vou Assumir que esta sempre vindo como 1

    uint willFlag = connectionFlags & 4;
    uint willQoS, willRetain;

    if(willFlag){
        if(verbose)
            printf("Will flag set.\n");

        willQoS = connectionFlags & 24;
        willRetain = connectionFlags & 32;

        if(verbose){
            printf("Will quality of service: %d\n", willQoS);
            printf("Will Retain: %d\n", willRetain);    
        }
        
    }
    else {
        willQoS = 0;
        willRetain = 0;
    }

    uint userNameFlag = connectionFlags & 128;
    uint passwordFlag = 0;

    if(userNameFlag){
        if(verbose)
            printf("Username flag set\n");

        passwordFlag = connectionFlags & 64;
        if(passwordFlag){
            if(verbose)
                printf("Password flag set\n");
        }
    }

    uint16_t keepAliveTime = (receivedCommunication[offset] << 8) | (receivedCommunication[offset + 1]);
    offset += 2;

    if(verbose)
        printf("Keep Alive time:%d\n", keepAliveTime);

    utf8String* clientIdentifier = decodeUtf8String(&receivedCommunication[offset]);
    offset += clientIdentifier->stringLength+2;

    if(verbose)
        printf("ClientIdentifier:%s\n", clientIdentifier->string);    

    if(willFlag){
        utf8String* willTopic = decodeUtf8String(&receivedCommunication[offset]);
        offset+=willTopic->stringLength+2;

        if(verbose)
            printf("Will Topic:%s\n", willTopic->string);

        utf8String* willMessage = decodeUtf8String(&receivedCommunication[offset]); //Nao eh exatamente uma string mas serve vai
        offset+= willMessage->stringLength+2;        

        if(verbose)
            printf("Will Message:%s\n", willMessage->string);

        free(willTopic->string);
        free(willTopic);

        free(willMessage->string);
        free(willMessage);
    }

    if(userNameFlag){
        utf8String* userName = decodeUtf8String(&receivedCommunication[offset]);
        offset+= userName->stringLength+2;

        if(verbose)
            printf("UserName: %s\n", userName->string);

        free(userName->string);
        free(userName);
    }

    if(passwordFlag){
        utf8String* password = decodeUtf8String(&receivedCommunication[offset]);
        offset+= password->stringLength+2;
        
        if(verbose)
            printf("Password: %s\n", password->string);

        free(password->string);
        free(password);
    }
    
    //TODO check clientidentifier and disconnect older if reused.
    connection->ClientIdentification=clientIdentifier->string;
    free(clientIdentifier);

    CONNACK(0, connfd);
}

void PINGREQ(activeConnection *connection, uint8_t flags, uint8_t* receivedCommunication, int remainingLength, int connfd){
    if(verbose)
        printf("Sending PINGRESP back o client\n");

    PINGRESP(connfd);
}

void SUBSCRIBE(activeConnection *connection, uint8_t flags, uint8_t* receivedCommunication, int remainingLength, int connfd){
    int offset = 0;

    if(flags != 2){        
        printf("Invalid flags! Close network connection. [MQTT-2.2.2-2]\n");
        close(connfd);
        return;
    }

    uint16_t packetIdentifier = (receivedCommunication[0] << 8) | (receivedCommunication[1]);    

    if(verbose)
        printf("Packet Identifier: %d\n", packetIdentifier);

    offset+=2;

    uint8_t returnCodes[MAXTOPICS];
    uint topicsCount =0;

    while(remainingLength - offset>0){  

        utf8String *topicString = decodeUtf8String(&receivedCommunication[offset]);

        if(verbose)
            printf("Topic chosen:%s\n", topicString->string);

        offset+=2 + topicString->stringLength;

        uint8_t qos = receivedCommunication[offset];
        offset++;

        if((qos >> 2) > 0){
            printf("Malformed QoS. Closing connection [MQTT-3-8.3-4]\n");
            close(connfd);
        }

        if(verbose)
            printf("QoS: %d\n", qos);
        
        //Por enquanto, implementando somente QoS 0. Se nao for QoS 0 retronar SUBACK Failure
        if(qos == 0){
        
            uint replace = 0;
            for(uint j=0;j<connection->interestedTopicsLength; j++){
                if(strcmp(connection->interestedTopics[j],topicString->string) == 0){
                    if(verbose)
                        printf("Topic was already subscribed. Replace.\n");
                    replace=1;
                    break;
                }
            }

            if(!replace){
                if(verbose)
                    printf("Subscribing to new topic.\n");

                connection->interestedTopics[connection->interestedTopicsLength] = malloc(topicString->stringLength * sizeof(char));

                //strncpy(connection->interestedTopics[connection->interestedTopicsLength], topicString->string, topicString->stringLength);
                connection->interestedTopics[connection->interestedTopicsLength] = topicString->string;
                (connection->interestedTopicsLength)++;                
            }

            returnCodes[topicsCount] = 0;
            topicsCount++;
        }
        else {
            returnCodes[topicsCount] = 128; //failure
            topicsCount++;
        }
        
        free(topicString);//the string itself pointer is still in use. Free only full structure.
    }    
    
    SUBACK(packetIdentifier, returnCodes, topicsCount, connfd);
}

void PUBLISH_RECEIVE(activeConnection *connection, uint8_t flags, uint8_t* receivedCommunication, int remainingLength, int connfd){
    int offset=0;

    //uint8_t dupFlag = flags >> 3;
    uint8_t qosLevel = (flags >> 1) & 3;
    //uint8_t retain = flags & 1;

    if(qosLevel == 3){
        printf("Invalid QoS Level. Closing connection. [MQTT-3.3.1-4]\n");
        close(connfd);
        return;
    }

    //Deixando a implementacao do Retain para depois, se der tempo.

    utf8String *topicString = decodeUtf8String(&receivedCommunication[offset]);

    if(verbose)
        printf("Topic of the message:%s\n", topicString->string);

    offset+=2 + topicString->stringLength;

    int messageLength = remainingLength - offset;

    receivedMessage* message = malloc(sizeof(receivedMessage));
    
    message->topicName = topicString->string;
    message->topicLength = topicString->stringLength;

    message->messageLength = messageLength; 
    message->message = malloc(messageLength*sizeof(uint8_t));
    
    for(int i=0;i<message->messageLength;i++){        
        (message->message)[i] = receivedCommunication[offset];
        offset++;
    }   
    
    addMessageToList(messagesStorage, message);        
    free(topicString);  

    //Como ep nao precisa ser implementado para QoS maior que 0, nao vou responder  
}

void PUBLISH_NOTIFY(activeConnection *connection, receivedMessage* message){
    int fullSize = message->messageLength + message->topicLength + 2 + 2; //full size is message size + topicLength + topic utf8 header (2 bytes) + FixedHeader (2 bytes)    

    uint8_t* fullMessage = malloc(fullSize*sizeof(uint8_t));
    fullMessage[0] = 48;
    fullMessage[1] = fullSize - 2;
    fullMessage[2] = message->topicLength >> 8;
    fullMessage[3] = message->topicLength & 255;

    int offset = 4;

    for(int i=0; i<message->topicLength; i++){
        fullMessage[4+i] = (uint8_t) message->topicName[i];
        offset++;
    }

    for(int i=0; i< message->messageLength; i++){
        fullMessage[offset+i] = message->message[i];        
    }

    if(verbose)
        printf("Notifing client\n");

    write(connection->connfd, fullMessage, fullSize);

    free(fullMessage);    
}

/***********************************************************************************/
/********************CLIENTS THREADS (Listener and Notifier *************************/

typedef struct {    
    int connfd;    
}  clientArgs;

void* connectedClientNotifier(void *args){
    activeConnection* clientConnection = (activeConnection*) args;
    int internalMessageIndex = messagesStorage->head;

    for(;;){        
        if(clientConnection->stopSignal != 0){
            return NULL; //listener is waiting for this thread to finish.
        }

        if(messagesStorage->head != internalMessageIndex){
            internalMessageIndex = (internalMessageIndex + 1) % MAXMESSAGESHISTORY;
            receivedMessage* message = messagesStorage->history[internalMessageIndex];
            
            for(int i=0; i<clientConnection->interestedTopicsLength; i++){
                if (strcmp(message->topicName, clientConnection->interestedTopics[i])==0){

                    if(verbose)
                        printf("Client is interested in this topic. Publishing message\n");

                    PUBLISH_NOTIFY(clientConnection, message);
                }
            }
        }
        sleep(1); 
    }
}

void* connectedClientListen (void *arg){
    //Buffer de entrada das mensagens
    uint8_t  receivedCommunication[MAXLINE + 1];
    //tamanho da mensagem
    ssize_t n;
    //Listener thread
    pthread_t listenerThread;


    /**** PROCESSO FILHO ****/
    if(verbose)
        printf("[Uma conexão aberta]\n");

    clientArgs *args = (clientArgs*) arg;

    activeConnection* connection = malloc(sizeof(activeConnection));
    connection->interestedTopicsLength = 0;
    connection->interestedTopics = malloc(MAXTOPICS*sizeof(char*));
    connection->currentHead=0;
    connection->connfd=args->connfd;   
    connection->stopSignal = 0; 

    pthread_create(&listenerThread, NULL, connectedClientNotifier, connection);   
       
    
    while ((n=read(args->connfd, receivedCommunication, MAXLINE)) > 0) {
        
        //fixedHeader
        uint8_t control = receivedCommunication[0];                
        remainingLengthStruct* remainingLength = decodeRemainingLength(&receivedCommunication[1]);

        if(verbose){
            printf("Remaining Length:%d\n", remainingLength->remainingLength);
            printf("Multiplier offset:%d\n", remainingLength->multiplierOffset);
        }
        

        uint8_t packetType = control >> 4;
        uint8_t flags = control & 15;                

        switch (packetType)
        {
            case 1:
                if(verbose)
                    printf("CONNECT control packet type\n");          

                CONNECT(connection, flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, args->connfd);
                break;
            case 3:
                if(verbose)
                    printf("PUBLISH control packet type\n");                    

                PUBLISH_RECEIVE(connection, flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, args->connfd);
                break;
            case 4:
                if(verbose)
                    printf("PUBACK control packet type\n");                    

                break;
            case 5:
                if(verbose)
                    printf("PUBREC control packet type\n");                    

                break;
            case 6:
                if(verbose)
                    printf("PUBREL control packet type\n");                    

                break;
            case 7:
                if(verbose)
                    printf("PUBCOMP control packet type\n");                    

                break;
            case 8:
                if(verbose)
                    printf("SUBSCRIBE control packet type\n"); 

                SUBSCRIBE(connection, flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, args->connfd);
                break;
            case 10:
                if(verbose)
                    printf("UNSUBSCRIBE control packet type\n");                    

                break;
            case 12:
                printf("PINGREQ control packet type\n");       
                PINGREQ(connection, flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, args->connfd);
                break;
            case 14:
                if(verbose)
                    printf("DISCONNECT control packet type\n");                    

                break;
            default:
                if(verbose)
                    printf("Unrecognized communication client to server: %d \n", packetType);                    
                break;
        }       

    }

    connection->stopSignal = 1;    

    pthread_join(listenerThread, NULL);

    free(args);
    free(connection->ClientIdentification);
    for(int i=0;i<connection->interestedTopicsLength;i++){
        free(connection->interestedTopics[i]);        
    }
    free(connection->interestedTopics);
    free(connection);

    
    if(verbose)
        printf("[Uma conexão fechada]\n");

    return NULL;
}


/***************************************************************************/

int main (int argc, char **argv) {
    /* Os sockets. Um que será o socket que vai escutar pelas conexões
     * e o outro que vai ser o socket específico de cada conexão */
    int listenfd, connfd;
    /* Informações sobre o socket (endereço e porta) ficam nesta struct */
    struct sockaddr_in servaddr;          
    
    //Estrutura de threads
    pthread_t threads[MAXSUBSCRIPTIONS];
    int subscriptions=0;
    //Lista circular compartilhada entre todos os processos    
    messagesStorage = createMessagesStorage();   
    
    
    if (argc < 2 || (argc == 3 && strcmp(argv[2], "-v") != 0)) {
        fprintf(stderr,"Uso: %s <Porta> [-v]\n",argv[0]);
        fprintf(stderr,"Vai rodar um servidor de echo na porta <Porta> TCP\n");
        fprintf(stderr,"Opções:\n");
        fprintf(stderr,"-v -> Modo verborrágico \n");
        exit(1);
    }

    if(argc == 3){
        verbose=1;
    }
    else {
        verbose=0;
    }

    /* Criação de um socket. É como se fosse um descritor de arquivo.
     * É possível fazer operações como read, write e close. Neste caso o
     * socket criado é um socket IPv4 (por causa do AF_INET), que vai
     * usar TCP (por causa do SOCK_STREAM), já que o MQTT funciona sobre
     * TCP, e será usado para uma aplicação convencional sobre a Internet
     * (por causa do número 0) */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket :(\n");
        exit(2);
    }

    /* Agora é necessário informar os endereços associados a este
     * socket. É necessário informar o endereço / interface e a porta,
     * pois mais adiante o socket ficará esperando conexões nesta porta
     * e neste(s) endereços. Para isso é necessário preencher a struct
     * servaddr. É necessário colocar lá o tipo de socket (No nosso
     * caso AF_INET porque é IPv4), em qual endereço / interface serão
     * esperadas conexões (Neste caso em qualquer uma -- INADDR_ANY) e
     * qual a porta. Neste caso será a porta que foi passada como
     * argumento no shell (atoi(argv[1]))
     */
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(atoi(argv[1]));
    if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
        perror("bind :(\n");
        exit(3);
    }

    /* Como este código é o código de um servidor, o socket será um
     * socket passivo. Para isto é necessário chamar a função listen
     * que define que este é um socket de servidor que ficará esperando
     * por conexões nos endereços definidos na função bind. */
    if (listen(listenfd, LISTENQ) == -1) {
        perror("listen :(\n");
        exit(4);
    }

    printf("[Servidor no ar. Aguardando conexões na porta %s]\n",argv[1]);
    printf("[Para finalizar, pressione CTRL+c ou rode um kill ou killall]\n");
   
    
	for (;;) {        
               
        if ((connfd = accept(listenfd, (struct sockaddr *) NULL, NULL)) == -1 ) {
            perror("accept :(\n");
            exit(5);
        }
      
        subscriptions++;
        clientArgs* args = malloc(sizeof(clientArgs));
        args->connfd = connfd;

        /*******Após receber conexão de cliente, iniciar thread com listener.********/
        pthread_create(&(threads[subscriptions]), NULL, connectedClientListen, args);                        
    }

    exit(0);
}
