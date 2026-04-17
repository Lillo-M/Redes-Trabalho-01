#include <arpa/inet.h>
#include <bits/stdc++.h>
#include <cstdio>
#include <netinet/in.h>
#include <ostream>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

#include <fstream>
#include <string>

#define DEFAULT_PORT 8080
#define TIMEOUT_MS 13
#define POLL_REQUESTS_TIMEOUT 5000
#define INIT_ESTIMATED_TIMEOUT CLOCKS_PER_SEC / 2
#define MAXLINE 1024
#define RECEIVE_BUFFER_SIZE 1024 * 1024

enum TransferFlags {
  ACK = (1 << 0),
  DATA = (1 << 1),
  FIN = (1 << 2),
  SYN = (1 << 3),
};

typedef struct TransfererHeader {
  uint32_t sequence;
  uint32_t ackNumber;
  uint16_t dataSize;
  uint16_t rwnd;
  uint8_t flags;
  char data[1024];
} TransfererHeader;

typedef struct PacketTimer {
  clock_t estimatedTimeout = INIT_ESTIMATED_TIMEOUT;
  clock_t ackClock = 0;
  clock_t sendClock = 0;
  clock_t timeoutClock = 0;
  uint32_t packetSequence = 0;
} PacketTimer;

bool compareFileSha256(const string &filename, const string &expectedHash) {
  string command = "sha256sum \"" + filename + "\"";

  array<char, 128> buffer;
  string output;

  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe)
    return false;

  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    output += buffer.data();
  }

  pclose(pipe);

  size_t firstSpace = output.find(' ');
  if (firstSpace == string::npos)
    return false;

  string fileHash = output.substr(0, firstSpace);

  return fileHash == expectedHash;
}

class FileWriter {
private:
  ofstream file;

public:
  bool openFile(const string &filename) {
    file.open(filename, ios::binary);

    return file.is_open();
  }

  bool writeBuffer(const char *buffer, size_t size) {
    if (!file.is_open())
      return false;

    file.write(reinterpret_cast<const char *>(buffer), size);

    return file.good();
  }

  void closeFile() {
    if (file.is_open())
      file.close();
  }
};

class Client {
public:
  string ipv4 = "";
  int port = DEFAULT_PORT;
  struct sockaddr_in serverAddress{};
  socklen_t len = sizeof(serverAddress);
  int serverSocket = -1;
  string filename = "";
  PacketTimer packetTimer;
  pollfd pfd{};
  TransfererHeader packetBuff{};

  void createSocket() {
    serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket < 0) {
      perror("socket creation failed");
      exit(EXIT_FAILURE);
    }
  }
  void setupServerAddress() {
    memset(&serverAddress, 0, sizeof(serverAddress));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);

    if (ipv4.empty()) {
      cout << "localhost\n";
      serverAddress.sin_addr.s_addr = INADDR_ANY;
    } else {
      if (inet_pton(AF_INET, ipv4.c_str(), &serverAddress.sin_addr) != 1) {
        cout << "invalid ip\n";
        exit(-1); // invalid IP
      }
    }
  }

  void setSocketReceiveBufferSize(const int receiveBufferSize) {
    setsockopt(serverSocket, SOL_SOCKET, SO_SNDBUF, &receiveBufferSize,
               sizeof(receiveBufferSize));
  }

  Client(int argc, char **argv) {
    if (argc < 2) {
      cout << "Insert server ip address: ";
      getline(cin, ipv4);

      cout << "Retrieve file named: ";
      cin >> filename;
    } else if (argc == 3) {
      handleApplicationArgs(argv);
    } else {
      argsUsageError();
      return;
    }

    string request = "GET " + filename;

    createSocket();

    setupServerAddress();

    setSocketReceiveBufferSize(RECEIVE_BUFFER_SIZE);

    int ret = tryHandshake();
    if (ret < 0) {
      cout << "handshake failed\n";
      exit(-1);
    }

    memccpy(packetBuff.data, request.c_str(), 0, 1023);
    // Send message to server
    sendto(serverSocket, &packetBuff, sizeof(packetBuff), MSG_CONFIRM,
           (const struct sockaddr *)&serverAddress, sizeof(serverAddress));
    cout << "Hello message sent.\n";

    TransfererHeader ack{0};

    int sequence = 0;

    FileWriter fileWriter;
    fileWriter.openFile(request);

    // Receive reply from server
    while (true) {
      TransfererHeader ackPacket;
      int n = recvfrom(serverSocket, &ackPacket, sizeof(ackPacket), MSG_WAITALL,
                       (struct sockaddr *)&serverAddress, &len);

      std::printf("Seq: %d, ack: %d, data: |`%s`|\n", ackPacket.sequence,
                  ackPacket.ackNumber, ackPacket.data);

      if (ackPacket.flags & TransferFlags::DATA) {

        if (ackPacket.sequence >= sequence) {
          if (ackPacket.sequence == sequence) {
            fileWriter.writeBuffer(ackPacket.data, ackPacket.dataSize);
          }

          ack.sequence = ackPacket.sequence;
          ack.ackNumber = ackPacket.sequence;
          ack.flags = TransferFlags::ACK;
          sendto(serverSocket, &ack, sizeof(ack), MSG_CONFIRM,
                 (const struct sockaddr *)&serverAddress,
                 sizeof(serverAddress));

          sequence += !(ackPacket.sequence > sequence);
        } else {
          ack.sequence = ackPacket.sequence;
          ack.flags = TransferFlags::ACK;
          sendto(serverSocket, &ack, sizeof(ack), MSG_CONFIRM,
                 (const struct sockaddr *)&serverAddress,
                 sizeof(serverAddress));
        }

      } else if (ackPacket.flags & TransferFlags::FIN) {
        fileWriter.closeFile();
        cout << "Server: Transmition ended checksum"
             << (compareFileSha256(request, string((char *)ackPacket.data))
                     ? "Success"
                     : "Failed")
             << " -> " << ackPacket.data << endl;
        ;
        break;
      }
    }

    // Close socket
    close(serverSocket);
    return;
  }

  void argsUsageError() {
    cout << "usage: ./client <ipv4>:<port> <filepath>\n";
    exit(-1);
  }

  void handleApplicationArgs(char **arguments) {
    string address(arguments[1]);
    regex pattern("^((((?!25?[6-9])[12]\\d|[1-9])?\\d\\.?\\b){4}):("
                  "[0-9]{1,5})$");

    smatch match;

    if (!regex_search(address, match, pattern)) {
      argsUsageError();
    }

    port = stoi(match[4]);

    if (65536 <= port) {
      cout << "invalid port number: " << port << endl;
      exit(-1);
    }

    ipv4 = match[1];

    filename = arguments[2];

    cout << "ipv4: " << ipv4 << "\nport: " << port << "\nfilename: " << filename
         << endl;
  }

  int tryHandshake() {
    TransfererHeader header;

    sendSyn();

    int ret = waitForEstabOrTimeout();

    if (ret < 0) {
      return ret;
    }

    receiveEstab(header);

    sendEstab(header);

    return 0;
  }

  void sendEstab(TransfererHeader &header) {
    TransfererHeader packet{.sequence = 0,
                            .ackNumber = header.sequence + 1,
                            .dataSize = 0,
                            .flags = (TransferFlags::ACK | TransferFlags::SYN),
                            .data = ""};

    sockSend(packet);
    initTimeout(packet.sequence);
    packetBuff = packet;
  }

  int receiveEstab(TransfererHeader &header) {
    ssize_t bytesReceived = sockReceive(header);

    if (bytesReceived <= 0) {
      cout << "Failed to receive\n";
      return -1;
    }

    if (header.flags != TransferFlags::SYN ||
        header.ackNumber != packetBuff.sequence + 1) {
      cout << "handshake failed\n";
      return -2;
    }

    return 0;
  }

  int waitForEstabOrTimeout() {
    int ret = poll(&pfd, 1, TIMEOUT_MS);

    int timeouts = 0;
    while (timeouts < 3) {
      ret = poll(&pfd, 1, TIMEOUT_MS);
      if (ret == 0) {
        return 0;
      }
      if (clock() >= packetTimer.timeoutClock) {
        timeouts++;
        initTimeout(packetTimer.packetSequence);
        sockSend(packetBuff);
      }
    }
    return -1;
  }

  void sendSyn() {
    TransfererHeader packet{.sequence = static_cast<uint32_t>(rand() % 100),
                            .ackNumber = 0,
                            .dataSize = 0,
                            .flags = TransferFlags::SYN,
                            .data = ""};
    sockSend(packet);
    packetBuff = packet;
  }

  ssize_t sockReceive(TransfererHeader &packet) {
    return recvfrom(serverSocket, &packet, sizeof(packet), MSG_WAITALL,
                    (struct sockaddr *)&serverAddress, &len);
  }

  void sockSend(TransfererHeader &data) {
    sendto(serverSocket, &data, sizeof(data), 0,
           (struct sockaddr *)&serverAddress, sizeof(serverAddress));
  }

  void resetTimeout() {
    packetTimer.timeoutClock = clock() + packetTimer.estimatedTimeout;
  }

  void initTimeout(uint32_t sequence) {
    packetTimer.packetSequence = sequence;
    packetTimer.sendClock = clock();
    resetTimeout();
  }

  void setupPollDescriptor() {
    pfd.fd = serverSocket;
    pfd.events = POLLIN;
  }
};

int main(int argc, char **argv) {
  Client client(argc, argv);
  return 0;
}
