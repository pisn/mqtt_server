all: mqtt_server.c
	gcc -Wall -o mqtt_server mqtt_server.c -lpthread

clean: 
	rm mqtt_server