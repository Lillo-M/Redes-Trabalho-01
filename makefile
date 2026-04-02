all:
	g++ client.cpp -o client
	g++ server.cpp -o server

clean:
	rm client server
