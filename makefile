all:
	g++ -ggdb client.cpp -o client
	g++ -ggdb server.cpp -o server -lssl -lcrypto

clean:
	rm client server
