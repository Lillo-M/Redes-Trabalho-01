#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using std::this_thread::sleep_for;

using namespace std;

#define PORT 8080
#define FAIL -1
#define TIMEOUT_MS 0
#define POLL_REQUESTS_TIMEOUT 5000
#define INIT_ESTIMATED_TIMEOUT CLOCKS_PER_SEC / 2
#define PAYLOAD_SIZE 1024

#define WINDOW_SIZE 100

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
  char data[PAYLOAD_SIZE];
} TransfererHeader;

typedef struct PacketTimer {
  clock_t estimatedTimeout = INIT_ESTIMATED_TIMEOUT;
  clock_t ackClock = 0;
  clock_t sendClock = 0;
  clock_t timeoutClock = 0;
  uint32_t packetSequence = 0;
} PacketTimer;

void closeSocketAtExit(int status, void *socket) {
  if (*((int *)socket) >= 0) {
    close(*((int *)socket));
    *((int *)socket) = -1;
    return;
  }
}

class Server {

public:
  pollfd pfd{};
  sockaddr_in serverAddress{};
  socklen_t clientLen = sizeof(serverAddress);
  int serverSocket = -1;
  sockaddr_in clientAddress{};
  ifstream file = ifstream();
  deque<TransfererHeader> nackedPacketsBuffer = deque<TransfererHeader>(1024);
  int nackedPacketsCount = 0;
  string ipv4 = "";
  PacketTimer packetTimer;
  int clientKeepAliveTimeout;

  void inputIpAddress() {
    cout << "Server address: ";
    getline(cin, ipv4);
    regex pattern("^(((?!25?[6-9])[12]\\d|[1-9])?\\d\\.?\\b){4}$");

    if (!regex_search(ipv4, pattern) && !ipv4.empty())
      exit(-1);
  }

  void setupServerAddress() {
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(PORT);

    if (ipv4.empty()) {
      cout << "Setting up server in localhost\n";
      serverAddress.sin_addr.s_addr = INADDR_ANY;
    } else {
      if (inet_pton(AF_INET, ipv4.c_str(), &serverAddress.sin_addr) != 1) {
        cout << "invalid ip\n";
        exit(-1); // invalid IP
      }
    }
  }

  void bindSocket() {
    if (bind(serverSocket, (struct sockaddr *)&serverAddress,
             sizeof(serverAddress)) == FAIL) {
      cout << "failed to bind\n";
      exit(-1);
    }
    on_exit(closeSocketAtExit, &serverSocket);
  }

  void setupPollDescriptor() {
    pfd.fd = serverSocket;
    pfd.events = POLLIN;
  }

  Server() {
    inputIpAddress();

    // creating socket
    serverSocket = socket(AF_INET, SOCK_DGRAM,
                          0); // DGRAM -> manual disse que DGRAM seria
                              // datagramas, suponho então que seja uma conexão
                              // udp, já que as outras são 'confiaveis'

    // specifying the address
    setupServerAddress();

    // binding socket.
    bindSocket();

    setupPollDescriptor();

    string request = pollRequest();

    cout << "Message from client: " << request << endl;

    handleRequest(request);

    // closing the socket.
    closeSocket();
  }

  void closeSocket() {
    if (serverSocket >= 0) {
      close(serverSocket);
    }
    serverSocket = -1;
  }

  unsigned long getFileSize(const string &filename) {
    return std::filesystem::file_size(filename);
  }

  bool fileExistsInRoot(const string &filename) {
    namespace fs = filesystem;

    fs::path path(filename);

    if (path.has_parent_path())
      return false;

    fs::path fullPath = fs::current_path() / path;

    return fs::exists(fullPath) && fs::is_regular_file(fullPath);
  }

  void openFile(const std::string &filename) {
    file.open(filename, ios::binary);
  }

  void closeFile() { file.close(); }

  /**
   * return 1 when read more than zero bytes;
   * return 0 when read less or equal zero bytes;
   * return -1 when file is null;
   * */
  int readFileChunk(vector<unsigned char> &buffer) {
    if (!file.is_open())
      return -1;

    file.read(reinterpret_cast<char *>(buffer.data()), buffer.size());

    std::streamsize bytesRead = file.gcount();

    return bytesRead;
  }

  TransfererHeader createPacket(vector<unsigned char> &data, int dataSize,
                                uint32_t sequence, uint8_t flags) {
    TransfererHeader packet;
    memccpy(packet.data, data.data(), 0, dataSize);

    packet.dataSize = dataSize;
    packet.sequence = sequence;
    packet.ackNumber = sequence;
    packet.flags = flags;

    return packet;
  }

  std::string sha256File(const std::string &filename) {
    std::string command = "sha256sum \"" + filename + "\"";

    std::array<char, 128> buffer;
    std::string result;

    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe)
      return "";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
      result += buffer.data();

    pclose(pipe);

    size_t space = result.find(' ');
    if (space == std::string::npos)
      return "";

    return result.substr(0, space);
  }

  void handleRequest(string &request) {
    string response;
    if (string::npos == request.find("GET")) {
      response = "Error: Bad request";
      cout << response << endl;
      return;
    }

    const int indexAfterGetAndWhitespace = 4;

    string filename =
        request.substr(indexAfterGetAndWhitespace, request.length());

    if (!fileExistsInRoot(filename)) {
      response = "Seu GET é uma merda\n";
      cout << response << endl;
      return;
    }

    string checksum = sha256File(filename);

    openFile(filename);

    streamFile(checksum);

    closeFile();
  }

  ssize_t sockReceive(TransfererHeader &header) {
    return recvfrom(serverSocket, &header, sizeof(header), 0,
                    (sockaddr *)&clientAddress, &clientLen);
  }

  void sendPacket(TransfererHeader &packet) {
    sockSend(packet);

    nackedPacketsBuffer.push_back(packet);
    nackedPacketsCount++;
  }

  void handlePacketLoss(TransfererHeader &packet) {
    if (packet.sequence == nackedPacketsBuffer[0].sequence - 1) {
      resendAllNackedPackets();
    }
  }

  void sendEstab(TransfererHeader &header) {
    TransfererHeader packet{.sequence = static_cast<uint32_t>(rand() % 100),
                            .ackNumber = header.sequence + 1,
                            .dataSize = 0,
                            .flags = TransferFlags::SYN,
                            .data = ""};

    sendPacket(packet);
    initTimeout(packet.sequence);
  }

  int receiveSyn(TransfererHeader &header) {
    ssize_t bytesReceived = sockReceive(header);

    if (bytesReceived <= 0) {
      cout << "Failed to receive\n";
      return -1;
    }

    if (header.flags != TransferFlags::SYN) {
      cout << "handshake failed\n";
      return -2;
    }
    return 0;
  }

  int receiveEstab(TransfererHeader &header) {
    ssize_t bytesReceived = sockReceive(header);

    if (bytesReceived <= 0) {
      cout << "Failed to receive\n";
      return -1;
    }

    if (header.flags != (TransferFlags::ACK | TransferFlags::SYN) ||
        header.ackNumber != packetTimer.packetSequence + 1) {
      cout << "handshake failed\n";
      return -2;
    }

    nackedPacketsBuffer.pop_back();
    nackedPacketsCount--;
    return 0;
  }

  int tryHandshake() {
    TransfererHeader header;

    int ret = receiveSyn(header);

    if (ret < 0) {
      return ret;
    }

    sendEstab(header);

    int timeouts = 0;
    do {
      waitForEstabOrTimeout();

      ret = receiveEstab(header);

      timeouts++;
      if (timeouts >= 100) {
        return -2;
      }
    } while (ret == -2);
    return ret;
  }

  int waitForEstabOrTimeout() {
    int ret = poll(&pfd, 1, TIMEOUT_MS);

    int timeouts = 0;
    while (timeouts < 3) {
      ret = poll(&pfd, 1, TIMEOUT_MS);
      if (ret > 0) {
        return 0;
      }
      if (clock() >= packetTimer.timeoutClock) {
        timeouts++;
        initTimeout(packetTimer.packetSequence);
        resendAllNackedPackets();
      }
    }
    return -1;
  }

  int waitForRequestOrTimeout() {
    int ret = poll(&pfd, 1, TIMEOUT_MS);

    int timeouts = 0;
    while (timeouts < 2) {
      ret = poll(&pfd, 1, POLL_REQUESTS_TIMEOUT);
      if (ret > 0) {
        return 0;
      }
    }
    return -1;
  }

  void waitForPackets() {
    int ret = poll(&pfd, 1, POLL_REQUESTS_TIMEOUT);

    while (ret <= 0) {
      ret = poll(&pfd, 1, POLL_REQUESTS_TIMEOUT);
    }
  }

  string pollRequest() {
    int handshake = -2;
    while (handshake < 0) {
      waitForPackets();

      handshake = tryHandshake();
    }

    return waitForRequest();
  }

  string waitForRequest() {
    int ret = waitForRequestOrTimeout();
    if (ret < 0) {

      return string("");
    }
    TransfererHeader packet{};
    ssize_t bytesReceived = sockReceive(packet);
    if (bytesReceived <= 0) {
      cout << "Failed to receive\n";
      return string("");
    }
    packet.data[PAYLOAD_SIZE - 1] = 0;
    return string(packet.data);
  }

  void pollAcks() {
    clientKeepAliveTimeout++;
    TransfererHeader header;
    ssize_t bytesReceived;

    int ret = poll(&pfd, 1, TIMEOUT_MS);

    while (ret > 0 && nackedPacketsCount > 0) {
      bytesReceived = recvfrom(serverSocket, &header, sizeof(header), 0,
                               (sockaddr *)&clientAddress, &clientLen);

      if (header.flags == TransferFlags::ACK) {

        bool acked = false;
        while (!nackedPacketsBuffer.empty() &&
               nackedPacketsBuffer.front().sequence <= header.sequence) {
          nackedPacketsBuffer.pop_front();
          nackedPacketsCount--;
          acked = true;
        }
        if (!acked) {
          handlePacketLoss(header);
        }
      }

      ret = poll(&pfd, 1, TIMEOUT_MS);
      clientKeepAliveTimeout = 0;
    }
  }

  bool windowIsFull() { return nackedPacketsCount >= WINDOW_SIZE; }

  void resendAllNackedPackets() {
    for (TransfererHeader &nacked : nackedPacketsBuffer) {
      sockSend(nacked);
    }
  }
  void handleFullWindow() {
    sleep_for(std::chrono::milliseconds(TIMEOUT_MS));

    pollAcks();

    if (!windowIsFull()) {
      return;
    }

    resendAllNackedPackets();
  }

  void streamFile(const string checksum) {
    TransfererHeader packet;
    vector<unsigned char> buffer(1024);

    int sequence = 0;

    while (!file.eof()) {

      pollAcks();

      if (windowIsFull()) {
        cout << "window is full\n";
        handleFullWindow();
        continue;
      }

      int bytesRead = readFileChunk(buffer);

      if (bytesRead <= 0) {
        cout << "failed to stream file: file not opened\n";
        exit(-1);
      }

      packet = createPacket(buffer, bytesRead, sequence, TransferFlags::DATA);

      sendPacket(packet);
      sequence++;
    }

    packet.sequence = sequence;
    packet.flags = TransferFlags::FIN;
    memccpy(packet.data, checksum.c_str(), 0, checksum.size());
    packet.data[checksum.size()] = 0;

    sockSend(packet);

    nackedPacketsBuffer.push_back(packet);
    nackedPacketsCount++;

    while (nackedPacketsCount > 0) {
      pollAcks();
      resendAllNackedPackets();
      sleep_for(std::chrono::milliseconds(TIMEOUT_MS));
    }
  }

  void resetTimeout() {
    packetTimer.timeoutClock = clock() + packetTimer.estimatedTimeout;
  }

  void initTimeout(uint32_t sequence) {
    packetTimer.packetSequence = sequence;
    packetTimer.sendClock = clock();
    resetTimeout();
  }

  void sockSend(TransfererHeader &data) {
    sendto(serverSocket, &data, sizeof(data), 0, (sockaddr *)&clientAddress,
           sizeof(clientAddress));
  }

  void sockSend(string str) {
    sendto(serverSocket, str.c_str(), str.size(), 0, (sockaddr *)&clientAddress,
           sizeof(clientAddress));
  }
};

int main() {
  Server server;
  return 0;
}
