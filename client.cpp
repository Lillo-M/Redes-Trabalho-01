#include <arpa/inet.h>
#include <bits/stdc++.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <netinet/in.h>
#include <ostream>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using std::this_thread::sleep_for;

using namespace std;

#include <fstream>
#include <string>

#define DEFAULT_PORT 8080
#define TIMEOUT_MS 0
#define WAIT_RESPONSE_TIMEOUT_MS 500
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

  void createFileDirectory(const string &filename) {
    namespace fs = filesystem;

    fs::path path(filename);

    path.remove_filename();

    fs::path fullPath = fs::current_path() / path;

    if (fs::exists(fullPath) || !path.has_parent_path()) {
      return;
    }

    fs::create_directories(fullPath);
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

void closeSocketAtExit(int status, void *socket) {
  if (*((int *)socket) >= 0) {
    close(*((int *)socket));
    *((int *)socket) = -1;
    return;
  }
}

class Client {
public:
  int losePacketChance = 0;
  string ipv4 = "";
  int port = DEFAULT_PORT;
  struct sockaddr_in serverAddress{};
  socklen_t len = sizeof(serverAddress);
  int serverSocket = -1;
  string filename = "";
  string filenameToSaveAs = "";
  PacketTimer packetTimer;
  pollfd pfd{};
  TransfererHeader packetBuff{};

  void createSocket() {
    serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket < 0) {
      perror("socket creation failed");
      exit(EXIT_FAILURE);
    }
    on_exit(closeSocketAtExit, &serverSocket);
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
        exit(EXIT_FAILURE); // invalid IP
      }
    }
  }

  void setSocketReceiveBufferSize(const int receiveBufferSize) {
    setsockopt(serverSocket, SOL_SOCKET, SO_SNDBUF, &receiveBufferSize,
               sizeof(receiveBufferSize));
  }

  void sendRequest() {
    int ret = poll(&pfd, 1, TIMEOUT_MS);

    int timeouts = 0;

    do {
      sockSend(packetBuff);
      sleep_for(std::chrono::milliseconds(WAIT_RESPONSE_TIMEOUT_MS));

      ret = poll(&pfd, 1, TIMEOUT_MS);

      timeouts++;
      if (timeouts > 100) {
        cout << "timeout: server stopped responding.\n" << endl;
        exit(-1);
      }
    } while (ret == 0);
  }

  void askRetransmition(int sequence) {
    std::printf("Asking retransmit of seq %u\n", sequence - 1);
    TransfererHeader ack{0};
    sendAck(sequence - 1);
  }

  void sendAck(int sequence) {
    TransfererHeader ack{0};
    ack.sequence = sequence;
    ack.ackNumber = sequence;
    ack.flags = TransferFlags::ACK;
    sockSend(ack);
  }

  Client(int argc, char **argv) {
    if (argc < 2) {
      cout << "Insert server ip address: ";
      getline(cin, ipv4);

      cout << "Retrieve file named: ";
      cin >> filename;
      std::printf("Save as (no input will save the file as `%s`): ",
                  filename.c_str());
      cin >> filenameToSaveAs;
    } else if (argc == 3 || argc == 4) {
      handleApplicationArgs(argv, argc);
    } else {
      argsUsageError();
      return;
    }

    string request = "GET " + filename;

    createSocket();

    setupServerAddress();

    setupPollDescriptor();

    setSocketReceiveBufferSize(RECEIVE_BUFFER_SIZE);

    int ret = tryHandshake();
    if (ret < 0) {
      cout << "handshake failed\n";
      exit(EXIT_FAILURE);
    }

    memccpy(packetBuff.data, request.c_str(), 0, 1023);

    sendRequest();

    TransfererHeader ack{0};

    int sequence = 0;

    FileWriter fileWriter;

    fileWriter.createFileDirectory(filenameToSaveAs);

    if (!fileWriter.openFile(filenameToSaveAs)) {
      printf("Failed to open file `%s`\n", filenameToSaveAs.c_str());
      exit(EXIT_FAILURE);
    }

    // Receive reply from server
    while (true) {
      TransfererHeader ackPacket;
      int n = recvfrom(serverSocket, &ackPacket, sizeof(ackPacket), MSG_WAITALL,
                       (struct sockaddr *)&serverAddress, &len);

      if (rand() % 100 < losePacketChance)
        continue;

      if (ackPacket.sequence == sequence) {
        if (ackPacket.flags == TransferFlags::DATA) {
          printf("Sequence: %d\n", ackPacket.sequence);
          fileWriter.writeBuffer(ackPacket.data, ackPacket.dataSize);
          sendAck(sequence);
          sequence++;
        } else if (ackPacket.flags & (FIN | DATA)) {
          fileWriter.closeFile();
          cout << "Server: Transmition ended checksum "
               << (compareFileSha256(filenameToSaveAs,
                                     string((char *)ackPacket.data))
                       ? "Succeeded"
                       : "Failed")
               << " -> " << ackPacket.data << endl;
          sendAck(sequence);
          sequence++;
          break;
        }
      } else if (n > 0 && ackPacket.sequence > sequence) {
        std::printf("...%u|%u\n", ackPacket.sequence, sequence);
        askRetransmition(sequence);
      }
    }

    finHandshake();

    // Close socket
    closeSocket();
    return;
  }

  void finHandshake() {
    TransfererHeader header;

    for (int i = 0; i <= 100 && receiveFin(header); i++) {
      sleep_for(chrono::milliseconds(WAIT_RESPONSE_TIMEOUT_MS));

      if (i == 100) {
        cout << "timeout: server stopped responding.\n" << endl;
        exit(-1);
      }
    }

    printf("headerfin: %u %u %u\n", header.flags, header.ackNumber,
           header.sequence);

    sendFinAck(header);
    printf("headerfinack: %u %u %u\n", packetBuff.flags, packetBuff.ackNumber,
           packetBuff.sequence);
    int ret = waitForPacketOrTimeout();

    if (ret < 0) {
      cout << "timeout: server stopped responding.\n" << endl;
      exit(-1);
    }

    receiveFinAck(header);
  }

  int receiveFin(TransfererHeader &header) {
    ssize_t bytesReceived = sockReceive(header);

    if (bytesReceived <= 0) {
      cout << "Failed to receive\n";
      return -1;
    }

    if (header.flags != TransferFlags::FIN) {
      cout << "FIN handshake failed\n";
      return -2;
    }

    return 0;
  }

  void sendFin(TransfererHeader &header) {
    TransfererHeader packet{.sequence = static_cast<uint32_t>(rand() % 100),
                            .ackNumber = 0,
                            .dataSize = 0,
                            .flags = TransferFlags::FIN,
                            .data = ""};

    sockSend(packet);
    packetBuff = packet;
  }

  void sendFinAck(TransfererHeader &header) {
    TransfererHeader packet{.sequence = static_cast<uint32_t>(rand() % 100),
                            .ackNumber = header.sequence + 1,
                            .dataSize = 0,
                            .flags = FIN | ACK,
                            .data = ""};

    sockSend(packet);
    packetBuff = packet;
  }

  int receiveFinAck(TransfererHeader &header) {
    ssize_t bytesReceived = sockPeek(header);

    if (bytesReceived <= 0) {
      cout << "Failed to peek\n";
      return -1;
    }

    if (header.flags != (TransferFlags::ACK & TransferFlags::FIN) ||
        header.ackNumber != packetBuff.sequence + 1) {
      cout << "FIN handshake failed\n";
      return -2;
    }

    if (sockReceive(header) <= 0) {
      cout << "Failed to receive\n";
      return -1;
    }

    return 0;
  }

  void closeSocket() {
    if (serverSocket >= 0) {
      close(serverSocket);
    }
    serverSocket = -1;
  }

  void argsUsageError() {
    cout << "usage: ./client <ipv4>:<port> <file_path> <save_as>\n"
            "save_as (optional): Is the directory/name to save the file in the "
            "client machine\n";
    exit(EXIT_FAILURE);
  }

  void handleApplicationArgs(char **arguments, int length) {
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
      exit(EXIT_FAILURE);
    }

    ipv4 = match[1];

    filename = arguments[2];

    filenameToSaveAs = length == 4 ? arguments[3] : filename;

    cout << "ipv4: " << ipv4 << "\nport: " << port << "\nfilename: " << filename
         << "\ndownloading as: " << filenameToSaveAs << endl;
  }

  int tryHandshake() {
    TransfererHeader header;

    sendSyn();

    int ret = waitForPacketOrTimeout();

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

  int waitForPacketOrTimeout() {
    int timeouts = 0;
    while (timeouts < 3) {
      int ret = poll(&pfd, 1, TIMEOUT_MS);
      if (ret > 0) {
        return 0;
      }

      sleep_for(std::chrono::milliseconds(WAIT_RESPONSE_TIMEOUT_MS));
      timeouts++;
      sockSend(packetBuff);
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

  ssize_t sockPeek(TransfererHeader &header) {
    return recvfrom(serverSocket, &header, sizeof(header), MSG_PEEK,
                    (sockaddr *)&serverAddress, &len);
  }

  ssize_t sockReceive(TransfererHeader &packet) {
    return recvfrom(serverSocket, &packet, sizeof(packet), MSG_WAITALL,
                    (struct sockaddr *)&serverAddress, &len);
  }

  void sockSend(TransfererHeader &data) {
    if (rand() % 100 < losePacketChance)
      return;
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
  srand(time(nullptr));
  Client client(argc, argv);
  return 0;
}
