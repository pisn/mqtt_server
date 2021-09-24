#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>

int main(int argc, char** argv){
    int i;
    int clients = atoi(argv[1]);    

    
    for(i=0; i<clients/2; i++){
        int pid = fork();
        int topic = rand() %4;

        if(pid==0){            
            
            switch(topic){
                case 0:
                    system("/usr/bin/mosquitto_sub -h localhost -p  8089 -t a -W 60");            
                    break;
                case 1:
                    system("/usr/bin/mosquitto_sub -h localhost -p  8089 -t b -W 60");            
                    break;
                case 2:
                    system("/usr/bin/mosquitto_sub -h localhost -p  8089 -t c -W 60");            
                    break;
                case 3:
                    system("/usr/bin/mosquitto_sub -h localhost -p  8089 -t d -W 60");            
                    break;
            }
            
            return 0;
        }                
    }    

    for(i=0; i<(clients - clients/2); i++){
        int pid = fork();
        int topic = rand() %4;

        int sleepTime = rand() % 50;

        if(pid==0){            

            sleep(sleepTime);
            
            switch(topic){
                case 0:
                    system("/usr/bin/mosquitto_pub -h localhost -p 8089 -t a -m blabla");            
                    break;
                case 1:
                    system("/usr/bin/mosquitto_pub -h localhost -p 8089 -t b -m lalu");            
                    break;
                case 2:
                    system("/usr/bin/mosquitto_pub -h localhost -p 8089 -t c -m xabu");            
                    break;
                case 3:
                    system("/usr/bin/mosquitto_pub -h localhost -p 8089 -t d -m tldr");            
                    break;
            }
            
            return 0;
        }                
    }


    return 0;    
}

