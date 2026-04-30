#include <arpa/inet.h>
#include <bits/stdc++.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <ostream>
#include <set>
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
#define PAYLOAD_SIZE 1420
#define RECEIVE_BUFFER_SIZE (1 << 13) * sizeof(TransfererHeader)

enum TransferFlags {
  ACK = (1 << 0),
  DATA = (1 << 1),
  FIN = (1 << 2),
  SYN = (1 << 3),
  SR = (1 << 4),
  ERROR = 0xFF,
};

typedef struct TransfererHeader {
  uint32_t sequence;
  uint32_t ackNumber;
  uint16_t dataSize;
  uint8_t checksum;
  uint8_t flags;
  char data[PAYLOAD_SIZE];
  bool operator<(const TransfererHeader &right) const {
    return this->sequence < right.sequence;
  }
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
  int lesserCount = 0;
  bool staleFeedbackSent = false;
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
  set<TransfererHeader> outOfOrderPackets = set<TransfererHeader>();

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
    setsockopt(serverSocket, SOL_SOCKET, SO_RCVBUF, &receiveBufferSize,
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
        exit(EXIT_FAILURE);
      }
    } while (ret == 0);
  }

  void askRetransmission(uint32_t sequence) {
    std::printf("Asking retransmit of seq %u\n", sequence - 1);
    sendAck(sequence - 1);
  }

  void sendAck(uint32_t sequence) {
    TransfererHeader ack{0};
    ack.sequence = sequence;
    ack.ackNumber = sequence;
    ack.flags = TransferFlags::ACK;
#ifdef SELECTIVE_REPEAT
    ack.flags |= SR;
#endif
    sockSend(ack);
  }

  Client(int argc, char **argv) {

    cout << "Sizeof packet " << sizeof(TransfererHeader) << "\n";
    if (argc < 2) {
      cout << "Insert server ip address: ";
      getline(cin, ipv4);

      cout << "Retrieve file named: ";
      cin >> filename;
      std::printf("Save as (no input will save the file as `%s`): ",
                  filename.c_str());
      cin >> filenameToSaveAs;
    } else if (argc >= 3 && argc <= 5) {
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

    memccpy(packetBuff.data, request.c_str(), 0, PAYLOAD_SIZE);

    sendRequest();

    uint32_t sequence = 0;

    FileWriter fileWriter;

    fileWriter.createFileDirectory(filenameToSaveAs);

    if (!fileWriter.openFile(filenameToSaveAs)) {
      printf("Failed to open file `%s`\n", filenameToSaveAs.c_str());
      exit(EXIT_FAILURE);
    }

    // Receive reply from server
    while (true) {
      TransfererHeader ackPacket;
      uint32_t timeoutCounter = 0;
      while (poll(&pfd, POLLIN, 1000) <= 0) {
        timeoutCounter++;
        if (timeoutCounter > 3) {
          cout << "timeout: server stopped responding.\n" << endl;
          exit(EXIT_FAILURE);
        }
      }
      int n = recvfrom(serverSocket, &ackPacket, sizeof(ackPacket), MSG_WAITALL,
                       (struct sockaddr *)&serverAddress, &len);

      if (rand() % 1000 < losePacketChance)
        continue;

      if (ackPacket.sequence == sequence && ackPacket.flags != ERROR) {
        lesserCount = 0;
        staleFeedbackSent = false;

        if (ackPacket.checksum !=
            calculateChecksum(ackPacket.data, ackPacket.dataSize)) {
          if (sequence > 0) {
            askRetransmission(sequence);
          }
          continue;
        }
        if (ackPacket.flags == TransferFlags::DATA) {
          printf("Sequence: %u\n", ackPacket.sequence);
          fileWriter.writeBuffer(ackPacket.data, ackPacket.dataSize);
          sendAck(sequence);
          sequence++;

#ifdef SELECTIVE_REPEAT

          int count = 0;
          for (auto &packet : outOfOrderPackets) {
            if (sequence != packet.sequence) {
              break;
            }
            printf("OOO - Sequence: %d\n", packet.sequence);
            fileWriter.writeBuffer(packet.data, packet.dataSize);
            sendAck(sequence);
            sequence++;
            count++;
          }
          sendAck(sequence - 1);
          auto first = outOfOrderPackets.begin();
          auto last = std::next(first, count);
          outOfOrderPackets.erase(first, last);

#endif

        } else if (ackPacket.flags & (FIN | DATA)) {
          if (ackPacket.checksum !=
              calculateChecksum(ackPacket.data, ackPacket.dataSize)) {
            askRetransmission(sequence);
            continue;
          }
          fileWriter.closeFile();
          cout << "Server: Transmition ended\n"
               << "checksum "
               << (compareFileSha256(filenameToSaveAs,
                                     string((char *)ackPacket.data))
                       ? "Succeeded"
                       : "Failed")
               << " -> " << ackPacket.data << endl;
          sendAck(sequence);
          sequence++;
          break;
        }
      } else if (n > 0 && ackPacket.flags & DATA && ackPacket.flags != ERROR) {
        std::printf("...%u|%u\n", ackPacket.sequence, sequence);

#ifdef SELECTIVE_REPEAT
        if (ackPacket.flags == TransferFlags::DATA &&
            ackPacket.sequence > sequence) {
          outOfOrderPackets.insert(ackPacket);
        }
#endif

        discardStalePackets();
        askRetransmission(sequence);
      } else if (ackPacket.flags == ERROR) {
        cout << "Bad request: ";
        fwrite(ackPacket.data, 1, ackPacket.dataSize, stdout);
        cout << "\n";
        break;
      }
    }

    finHandshake();

    // Close socket
    closeSocket();
    return;
  }

  void discardStalePackets() {
    TransfererHeader tmp;
    // Drena tudo que está no buffer sem bloquear
    while (poll(&pfd, 1, 0) > 0) {
      recvfrom(serverSocket, &tmp, sizeof(tmp), 0,
               (struct sockaddr *)&serverAddress, &len);
    }
  }

  void finHandshake() {
    TransfererHeader header;

    for (int i = 0; i <= 100 && receiveFin(header); i++) {
      sleep_for(chrono::milliseconds(WAIT_RESPONSE_TIMEOUT_MS));

      if (i == 100) {
        cout << "timeout: server stopped responding.\n" << endl;
        exit(EXIT_FAILURE);
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
      exit(EXIT_FAILURE);
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
    cout
        << "usage: ./client <ipv4>:<port> <file_path> <save_as> "
           "<test_packet_loss>\n"
           "save_as (optional): Is the directory/name to save the file in the "
           "client machine\n"
           "test_packet_loss is the amount per 1000 of packets to be dropped\n";
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

    filenameToSaveAs = length >= 4 ? arguments[3] : filename;

    if (length >= 5) {
      losePacketChance = atoi(arguments[4]);
      printf("Losing packets for testing enabled: %.1f %%\n",
             ((float)losePacketChance) / 10.f);
    }

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
    if (rand() % 1000 < losePacketChance)
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

  // https://stackoverflow.com/questions/51752284/how-to-calculate-crc8-in-c
  uint8_t calculateChecksum(char *data, int size) {
    uint8_t crc = 0x00;

    for (size_t i = 0; i < size; i++) {
      crc ^= data[i];

      for (int j = 0; j < 8; j++) {
        if (crc & 0x80)
          crc = (crc << 1) ^ 0x07;
        else
          crc <<= 1;
      }
    }

    return crc;
  }
};

int main(int argc, char **argv) {
  srand(time(nullptr));
  Client client(argc, argv);
  return 0;
}
