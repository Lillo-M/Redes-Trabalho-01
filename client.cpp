// Client side implementation of UDP client-server model

#include <arpa/inet.h>
#include <bits/stdc++.h>
#include <netinet/in.h>
#include <ostream>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

#define PORT 8080
#define MAXLINE 1024

enum TransferFlags {
  ACK = 0,
  DATA = 1,
  END = 2,
};

struct TransfererHeader {
  uint32_t sequence;
  uint16_t dataSize;
  uint8_t flags;
  unsigned char data[1024];
};

struct TransfereeHeader {
  uint32_t sequence;
  uint8_t flags;
};

int main() {
  std::string ipv4 = "";
  cout << "Server address: ";
  getline(cin, ipv4);
  std::regex pattern("^(((?!25?[6-9])[12]\\d|[1-9])?\\d\\.?\\b){4}$");

  if (!regex_search(ipv4, pattern) && !ipv4.empty())
    return -1;

  int sockfd;
  struct TransfererHeader buffer;

  string filename;

  cout << "Retrieve file named: ";
  cin >> filename;

  string request = "GET " + filename;
  struct sockaddr_in serverAddress;

  // Create UDP socket
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket creation failed");
    exit(EXIT_FAILURE);
  }

  memset(&serverAddress, 0, sizeof(serverAddress));

  serverAddress.sin_family = AF_INET;
  serverAddress.sin_port = htons(PORT);

  if (ipv4.empty()) {
    cout << "localhost\n";
    serverAddress.sin_addr.s_addr = INADDR_ANY;
  } else {
    if (inet_pton(AF_INET, ipv4.c_str(), &serverAddress.sin_addr) != 1) {
      cout << "invalid ip\n";
      return -1; // invalid IP
    }
  }

  socklen_t len = sizeof(serverAddress);

  // Send message to server
  sendto(sockfd, request.c_str(), request.length(), MSG_CONFIRM,
         (const struct sockaddr *)&serverAddress, sizeof(serverAddress));
  cout << "Hello message sent.\n";

  struct TransfereeHeader ack;

  int sequence = 0;
  // Receive reply from server
  while (true) {
    int n = recvfrom(sockfd, &buffer, sizeof(buffer), MSG_WAITALL,
                     (struct sockaddr *)&serverAddress, &len);

    if (buffer.flags == TransferFlags::DATA) {

      ack.sequence = sequence;
      ack.flags = TransferFlags::ACK;

      if (buffer.sequence >= sequence) {
        sendto(sockfd, &ack, sizeof(ack), MSG_CONFIRM,
               (const struct sockaddr *)&serverAddress, sizeof(serverAddress));

        sequence += !(buffer.sequence > sequence);
      } else {
        ack.sequence = buffer.sequence;
        sendto(sockfd, &ack, sizeof(ack), MSG_CONFIRM,
               (const struct sockaddr *)&serverAddress, sizeof(serverAddress));
      }

    } else if (buffer.flags == TransferFlags::END) {
      cout << "Server: Transmition ended checksum -> " << buffer.data
           << std::endl;
      break;
    }
  }

  // Close socket
  close(sockfd);

  return 0;
}
