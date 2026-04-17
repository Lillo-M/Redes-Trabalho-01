all:
	g++ -ggdb client.cpp -o client
	g++ -ggdb server.cpp -o server

clean:
	rm client server
