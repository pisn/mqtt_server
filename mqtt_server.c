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

void CONNECT(uint8_t flags, uint8_t* receivedCommunication, int remainingLength, int connfd){
    if(flags != 0){        
        printf("Invalid flags! Close network connection. [MQTT-2.2.2-2]");
        close(connfd);
        return;
    }

    uint16_t protocolNameLength = (receivedCommunication[0] << 8) | receivedCommunication[1];

    char* protocolName = malloc(protocolNameLength);

    for(uint16_t i=0; i<protocolNameLength; i++){
        protocolName[i] = receivedCommunication[2+i];
    }

    printf("Protocol Name:%s\n", protocolName);

    if(strcmp(protocolName,"MQTT") != 0){
        printf("Unrecognized protocol. Closing connection. [MQTT-3.1.2-1]\n");
        close(connfd); //protocolo nao reconhecido
        return;
    }

    uint8_t protocolLevel = receivedCommunication[2 + protocolNameLength];

    if(protocolLevel != 4){
        printf("Unrecognized protocol level %d. [MQTT-3.1.2-2]", protocolLevel);
        //CONNACK 1
        close(connfd);
        return;
    }

    uint8_t connectionFlags = receivedCommunication[3 + protocolNameLength];

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

    uint16_t keepAliveTime = (receivedCommunication[4 + protocolNameLength] << 8) | (receivedCommunication[5 + protocolNameLength]);
    printf("Keep Alive time:%d\n", keepAliveTime);

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
                        CONNECT(flags, &receivedCommunication[2+remainingLength->multiplierOffset], remainingLength->remainingLength, connfd);
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
