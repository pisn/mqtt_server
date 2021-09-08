/* Por Prof. Daniel Batista <batista@ime.usp.br>
 * Em 4/4/2021
 * 
 * Um código simples de um servidor de eco a ser usado como base para
 * o EP1. Ele recebe uma linha de um cliente e devolve a mesma linha.
 * Teste ele assim depois de compilar:
 * 
 * ./mac5910-servidor-exemplo-ep1 8000
 * 
 * Com este comando o servidor ficará escutando por conexões na porta
 * 8000 TCP (Se você quiser fazer o servidor escutar em uma porta
 * menor que 1024 você precisará ser root ou ter as permissões
 * necessáfias para rodar o código com 'sudo').
 *
 * Depois conecte no servidor via telnet. Rode em outro terminal:
 * 
 * telnet 127.0.0.1 8000
 * 
 * Escreva sequências de caracteres seguidas de ENTER. Você verá que o
 * telnet exibe a mesma linha em seguida. Esta repetição da linha é
 * enviada pelo servidor. O servidor também exibe no terminal onde ele
 * estiver rodando as linhas enviadas pelos clientes.
 * 
 * Obs.: Você pode conectar no servidor remotamente também. Basta
 * saber o endereço IP remoto da máquina onde o servidor está rodando
 * e não pode haver nenhum firewall no meio do caminho bloqueando
 * conexões na porta escolhida.
 */

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

#define LISTENQ 1
#define MAXDATASIZE 100
#define MAXLINE 65000
#define MAXTOPICS 200

typedef struct {
    char* ClientIdentification;
    //botar tempo do keepAlive depois
    char** interestedTopics;
    uint interestedTopicsLength;

} activeConnection;

typedef struct {
    int remainingLength, multiplierOffset;
} remainingLengthStruct;

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
    printf("Size:%d\n", stringLength);

    utf8String* returnString = malloc(sizeof(utf8String));
    returnString->stringLength = stringLength;
    returnString->string = malloc(stringLength*sizeof(char));

    for(uint16_t i=0; i<stringLength;i++){
        returnString->string[i] = utf8EncodedStream[2+i];
    }

    return returnString;
}

void CONNACK(uint8_t returnCode, int connfd){
    uint8_t responseStream[4]; //CONNACK tem tamanho fixo, nao tem payload

    responseStream[0] = 32;
    responseStream[1] = 2;
    responseStream[2] = 0; //Por enquanto nao estou tratando SessionPresent.
    responseStream[3] = returnCode;

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

    printf("Writing SUBACK\n");
    write(connfd, responseStream, returnCodeLength + 4);

    free(responseStream);    
}

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

    printf("Protocol Name:%s\n", protocolName);

    if(strcmp(protocolName,"MQTT") != 0){
        printf("Unrecognized protocol. Closing connection. [MQTT-3.1.2-1]\n");
        close(connfd); //protocolo nao reconhecido
        return;
    }

    uint8_t protocolLevel = receivedCommunication[offset];
    offset++;

    if(protocolLevel != 4){
        printf("Unrecognized protocol level %d. [MQTT-3.1.2-2]", protocolLevel);
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
        printf("Will flag set.\n");

        willQoS = connectionFlags & 24;
        willRetain = connectionFlags & 32;

        printf("Will quality of service: %d\n", willQoS);
        printf("Will Retain: %d\n", willRetain);
    }
    else {
        willQoS = 0;
        willRetain = 0;
    }

    uint userNameFlag = connectionFlags & 128;
    uint passwordFlag = 0;

    if(userNameFlag){
        printf("Username flag set\n");

        passwordFlag = connectionFlags & 64;
        if(passwordFlag){
            printf("Password flag set\n");
        }
    }

    uint16_t keepAliveTime = (receivedCommunication[offset] << 8) | (receivedCommunication[offset + 1]);
    offset += 2;
    printf("Keep Alive time:%d\n", keepAliveTime);

    utf8String* clientIdentifier = decodeUtf8String(&receivedCommunication[offset]);
    offset += clientIdentifier->stringLength+2;
    printf("ClientIdentifier:%s\n", clientIdentifier->string);    

    if(willFlag){
        utf8String* willTopic = decodeUtf8String(&receivedCommunication[offset]);
        offset+=willTopic->stringLength+2;

        printf("Will Topic:%s\n", willTopic->string);

        utf8String* willMessage = decodeUtf8String(&receivedCommunication[offset]); //Nao eh exatamente uma string mas serve vai
        offset+= willMessage->stringLength+2;        

        printf("Will Message:%s\n", willMessage->string);
    }

    if(userNameFlag){
        utf8String* userName = decodeUtf8String(&receivedCommunication[offset]);
        offset+= userName->stringLength+2;

        printf("UserName: %s\n", userName->string);
    }

    if(passwordFlag){
        utf8String* password = decodeUtf8String(&receivedCommunication[offset]);
        offset+= password->stringLength+2;
        
        printf("Password: %s\n", password->string);
    }
    
    //TODO check clientidentifier and disconnect older if reused.
    connection->ClientIdentification=clientIdentifier->string;

    CONNACK(0, connfd);
}

void SUBSCRIBE(activeConnection *connection, uint8_t flags, uint8_t* receivedCommunication, int remainingLength, int connfd){
    int offset = 0;

    if(flags != 2){        
        printf("Invalid flags! Close network connection. [MQTT-2.2.2-2]");
        close(connfd);
        return;
    }

    uint16_t packetIdentifier = (receivedCommunication[0] << 8) | (receivedCommunication[1]);    
    printf("Packet Identifier: %d\n", packetIdentifier);
    offset+=2;

    uint8_t returnCodes[MAXTOPICS];
    uint topicsCount =0;

    while(remainingLength - offset>0){
        printf("Offset: %d\n", offset);
        printf("Remaining Length: %d\n", remainingLength);

        utf8String *topicString = decodeUtf8String(&receivedCommunication[offset]);
        printf("Topic chosen:%s\n", topicString->string);
        printf("Topic Length:%d\n", topicString->stringLength);
        offset+=2 + topicString->stringLength;

        uint8_t qos = receivedCommunication[offset];
        offset++;

        if((qos >> 2) > 0){
            printf("Malformed QoS. Closing connection [MQTT-3-8.3-4]\n");
            close(connfd);
        }

        printf("QoS: %d\n", qos);
        
        //Por enquanto, implementando somente QoS 0. Se nao for QoS 0 retronar SUBACK Failure
        if(qos == 0){
        
            uint replace = 0;
            for(uint j=0;j<connection->interestedTopicsLength; j++){
                if(strcmp(connection->interestedTopics[j],topicString->string) == 0){
                    printf("Topic was already subscribed. Replace.");
                    replace=1;
                    break;
                }
            }

            if(!replace){
                printf("Subscribing to new topic.\n");
                connection->interestedTopics[connection->interestedTopicsLength] = malloc(topicString->stringLength * sizeof(char));

                strncpy(connection->interestedTopics[connection->interestedTopicsLength], topicString->string, topicString->stringLength);
                (connection->interestedTopicsLength)++;                
            }

            returnCodes[topicsCount] = 0;
            topicsCount++;
        }
        else {
            returnCodes[topicsCount] = 128; //failure
            topicsCount++;
        }
    }

    SUBACK(packetIdentifier, returnCodes, topicsCount, connfd);

}


int main (int argc, char **argv) {
    /* Os sockets. Um que será o socket que vai escutar pelas conexões
     * e o outro que vai ser o socket específico de cada conexão */
    int listenfd, connfd;
    /* Informações sobre o socket (endereço e porta) ficam nesta struct */
    struct sockaddr_in servaddr;
    /* Retorno da função fork para saber quem é o processo filho e
     * quem é o processo pai */
    pid_t childpid;
    /* Armazena linhas recebidas do cliente */
    uint8_t  receivedCommunication[MAXLINE + 1];
    /* Armazena o tamanho da string lida do cliente */
    ssize_t n;
   
    if (argc != 2) {
        fprintf(stderr,"Uso: %s <Porta>\n",argv[0]);
        fprintf(stderr,"Vai rodar um servidor de echo na porta <Porta> TCP\n");
        exit(1);
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
   
    /* O servidor no final das contas é um loop infinito de espera por
     * conexões e processamento de cada uma individualmente */
	for (;;) {
        /* O socket inicial que foi criado é o socket que vai aguardar
         * pela conexão na porta especificada. Mas pode ser que existam
         * diversos clientes conectando no servidor. Por isso deve-se
         * utilizar a função accept. Esta função vai retirar uma conexão
         * da fila de conexões que foram aceitas no socket listenfd e
         * vai criar um socket específico para esta conexão. O descritor
         * deste novo socket é o retorno da função accept. */
        if ((connfd = accept(listenfd, (struct sockaddr *) NULL, NULL)) == -1 ) {
            perror("accept :(\n");
            exit(5);
        }
      
        /* Agora o servidor precisa tratar este cliente de forma
         * separada. Para isto é criado um processo filho usando a
         * função fork. O processo vai ser uma cópia deste. Depois da
         * função fork, os dois processos (pai e filho) estarão no mesmo
         * ponto do código, mas cada um terá um PID diferente. Assim é
         * possível diferenciar o que cada processo terá que fazer. O
         * filho tem que processar a requisição do cliente. O pai tem
         * que voltar no loop para continuar aceitando novas conexões.
         * Se o retorno da função fork for zero, é porque está no
         * processo filho. */
        if ( (childpid = fork()) == 0) {
            /**** PROCESSO FILHO ****/
            printf("[Uma conexão aberta]\n");

            activeConnection* connection = malloc(sizeof(activeConnection));
            connection->interestedTopicsLength = 0;
            connection->interestedTopics = malloc(MAXTOPICS*sizeof(char*));

            /* Já que está no processo filho, não precisa mais do socket
             * listenfd. Só o processo pai precisa deste socket. */
            close(listenfd);
         
            /* Agora pode ler do socket e escrever no socket. Isto tem
             * que ser feito em sincronia com o cliente. Não faz sentido
             * ler sem ter o que ler. Ou seja, neste caso está sendo
             * considerado que o cliente vai enviar algo para o servidor.
             * O servidor vai processar o que tiver sido enviado e vai
             * enviar uma resposta para o cliente (Que precisará estar
             * esperando por esta resposta) 
             */

            /* ========================================================= */
            /* ========================================================= */
            /*                         EP1 INÍCIO                        */
            /* ========================================================= */
            /* ========================================================= */
            /* TODO: É esta parte do código que terá que ser modificada
             * para que este servidor consiga interpretar comandos MQTT  */
            while ((n=read(connfd, receivedCommunication, MAXLINE)) > 0) {
                
                //fixedHeader
                uint8_t control = receivedCommunication[0];                
                remainingLengthStruct* remainingLength = decodeRemainingLength(&receivedCommunication[1]);

                printf("Remaining Length:%d\n", remainingLength->remainingLength);
                printf("Multiplier offset:%d\n", remainingLength->multiplierOffset);

                uint8_t packetType = control >> 4;
                uint8_t flags = control & 15;                

                switch (packetType)
                {
                    case 1:
                        printf("CONNECT control packet type\n");          
                        CONNECT(connection, flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, connfd);
                        break;
                    case 3:
                        printf("PUBLISH control packet type\n");                    
                        break;
                    case 4:
                        printf("PUBACK control packet type\n");                    
                        break;
                    case 5:
                        printf("PUBREC control packet type\n");                    
                        break;
                    case 6:
                        printf("PUBREL control packet type\n");                    
                        break;
                    case 7:
                        printf("PUBCOMP control packet type\n");                    
                        break;
                    case 8:
                        printf("SUBSCRIBE control packet type\n"); 
                        SUBSCRIBE(connection, flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, connfd);
                        break;
                    case 10:
                        printf("UNSUBSCRIBE control packet type\n");                    
                        break;
                    case 12:
                        printf("PINGREQ control packet type\n");                    
                        break;
                    case 14:
                        printf("DISCONNECT control packet type\n");                    
                        break;
                    default:
                        printf("Unrecognized communication client to server: %d \n", packetType);                    
                        break;
                }

            }
            /* ========================================================= */
            /* ========================================================= */
            /*                         EP1 FIM                           */
            /* ========================================================= */
            /* ========================================================= */

            /* Após ter feito toda a troca de informação com o cliente,
             * pode finalizar o processo filho */
            printf("[Uma conexão fechada]\n");
            exit(0);
        }
        else
            /**** PROCESSO PAI ****/
            /* Se for o pai, a única coisa a ser feita é fechar o socket
             * connfd (ele é o socket do cliente específico que será tratado
             * pelo processo filho) */
            close(connfd);
    }
    exit(0);
}
