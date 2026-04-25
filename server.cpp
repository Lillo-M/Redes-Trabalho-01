#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
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
#include <ostream>
#include <poll.h>
#include <pthread.h>
#include <regex>
#include <signal.h>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

using std::this_thread::sleep_for;

using namespace std;
using namespace std::chrono;

#define PORT 8080
#define FAIL -1
#define TIMEOUT_MS 0
#define POLL_REQUESTS_TIMEOUT 5000
#define WAIT_RESPONSE_TIMEOUT_MS 50
#define INIT_ESTIMATED_TIMEOUT CLOCKS_PER_SEC / 2
#define PAYLOAD_SIZE 536

#define WINDOW_SIZE 512

enum TransferFlags {
  ACK = (1 << 0),
  DATA = (1 << 1),
  FIN = (1 << 2),
  SYN = (1 << 3),
};

enum State { SlowStart, CongestionAvoidance, FastRecovery };

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
  deque<TransfererHeader> nackedPacketsBuffer =
      deque<TransfererHeader>(WINDOW_SIZE);
  int nackedPacketsCount = 0;
  string ipv4 = "";
  PacketTimer packetTimer;
  int clientKeepAliveTimeout;
  uint32_t fileRemaining = 0;
  uint32_t fileSize = 0;
  static uint32_t cwnd;
  static uint32_t ssthresh;
  static State state;
  static Server *instance;
  struct itimerval timer{0};
  steady_clock::time_point rttBegin = steady_clock::now();
  uint32_t measuringSeq = -1;
  bool measuring = false;

  Server() {
    if (!instance) {
      instance = this;
    }

    signal(SIGALRM, Server::timeoutHandler);

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

    pollConections();

    string request = waitForRequest();

    cout << "Message from client: " << request << endl;

    handleRequest(request);

    TransfererHeader header;
    int ret = poll(&pfd, 1, TIMEOUT_MS);
    while (ret > 0) {
      sockReceive(header);
      ret = poll(&pfd, 1, TIMEOUT_MS);
    }

    finHandshake();
    sockReceive(header);

    // closing the socket.
    closeSocket();
  }

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

  static void timeoutHandler(int signum) {
    state = SlowStart;
    ssthresh = cwnd / 2;
    setCwnd(1);
    instance->timedOut();
    printf("Timeout: State changed to Slow Start | signum '%d'\n", signum);
  }

  static void setCwnd(uint32_t value) {
    if (value > WINDOW_SIZE) {
      cwnd = WINDOW_SIZE;
      return;
    }
    if (value <= 0U) {
      cwnd = 1;
      return;
    }
    cwnd = value;
  }

  void startTimer() {
    if (setitimer(ITIMER_REAL, &timer, NULL) == -1) {
      perror("Error calling setitimer()");
      exit(1);
    }
  }
  void setTimer(time_t us) {
    int seconds = us / (1000 * 1000);
    timer.it_value.tv_sec = seconds;
    timer.it_value.tv_usec = us % (1000 * 1000);

    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
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
    file.seekg(0, ios::end);
    fileRemaining = file.tellg();
    fileSize = fileRemaining;
    file.seekg(0);
    file.clear();
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
    memcpy(packet.data, data.data(), dataSize);

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

  ssize_t sockPeek(TransfererHeader &header) {
    return recvfrom(serverSocket, &header, sizeof(header), MSG_PEEK,
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
    ssize_t bytesReceived = sockPeek(header);

    if (bytesReceived <= 0) {
      cout << "Failed to peek\n";
      return -1;
    }

    if (header.flags != TransferFlags::SYN) {
      cout << "handshake failed\n";
      return -2;
    }

    if (sockReceive(header) <= 0) {
      cout << "Failed to receive\n";
      return -1;
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

  void sendFin() {
    TransfererHeader packet{.sequence = static_cast<uint32_t>(rand() % 100),
                            .ackNumber = 0,
                            .dataSize = 0,
                            .flags = TransferFlags::FIN,
                            .data = ""};

    printf("packet: %u %u %u\n", packet.flags, packet.ackNumber,
           packet.sequence);

    sendPacket(packet);
    initTimeout(packet.sequence);
  }

  int receiveFinAck(TransfererHeader &header) {
    ssize_t bytesReceived = sockReceive(header);

    if (bytesReceived <= 0) {
      cout << "Failed to receive\n";
      return -1;
    }

    while (header.flags == ACK) {
      ssize_t bytesReceived = sockReceive(header);
    }

    printf("header: %u %u %u\n", header.flags, header.ackNumber,
           packetTimer.packetSequence);
    if (header.flags != (TransferFlags::ACK | TransferFlags::FIN) ||
        header.ackNumber != packetTimer.packetSequence + 1) {
      cout << "FIN handshake failed\n";
      return -2;
    }

    return 0;
  }

  void finHandshake() {
    TransfererHeader header;

    sendFin();

    for (int i = 0; i <= 100 && receiveFinAck(header); i++) {
      sleep_for(chrono::milliseconds(WAIT_RESPONSE_TIMEOUT_MS));

      if (i == 100) {
        cout << "timeout: client stopped responding.\n" << endl;
        exit(-1);
      }
    }

    sendFinAck(header);
  }

  void sendFinAck(TransfererHeader &header) {
    TransfererHeader packet{.sequence = 0,
                            .ackNumber = header.sequence + 1,
                            .dataSize = 0,
                            .flags = ACK & FIN,
                            .data = ""};

    sendPacket(packet);
    initTimeout(packet.sequence);
  }

  int tryThreeWayHandshake() {
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
    nackedPacketsBuffer.pop_front();
    nackedPacketsCount--;
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

  void pollConections() {
    int handshake = -2;
    while (handshake < 0) {
      waitForPackets();

      handshake = tryThreeWayHandshake();
    }
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

  void setTimeout(bool hasTimedOut, uint32_t sampleRTT = 0) {
    float alpha = 1.f / 8;
    float beta = 1.f / 4;

    struct itimerval curr_value;
    if (getitimer(ITIMER_REAL, &curr_value) == -1) {
      perror("Error calling getitimer()");
      exit(1);
    }

    static uint32_t lastSample = 500;
    if (sampleRTT == 0) {
      sampleRTT = lastSample;
    } else {
      lastSample = sampleRTT;
    }

    static time_t estimatedRTT = 500 * 4 / 3, devRTT = 0, lastTimeoutLen = 0;

    if (hasTimedOut) {
      estimatedRTT *= 2;
    }

    estimatedRTT = (1 - alpha) * estimatedRTT + alpha * sampleRTT;

    devRTT = (1 - beta) * devRTT + beta * abs(sampleRTT - estimatedRTT);

    lastTimeoutLen = (estimatedRTT + 4 * devRTT);

    if (lastTimeoutLen > 1000) {
      lastTimeoutLen = 999;
    }

    setTimer(lastTimeoutLen * 1000);
    startTimer();
  }

  void acknowledgePacket(TransfererHeader &header) {
    switch (state) {
    case SlowStart:
      setCwnd(cwnd + 1);
      break;
    case CongestionAvoidance:
      setCwnd(cwnd + PAYLOAD_SIZE / cwnd);
      break;
    case FastRecovery:
      setCwnd(ssthresh + 1);
      avoidCongestion();
      break;
    }

    printf("Ack Received, cwnd `%u`\n", cwnd);

    while (!nackedPacketsBuffer.empty() &&
           nackedPacketsBuffer.front().sequence <= header.sequence) {
      nackedPacketsBuffer.pop_front();
      nackedPacketsCount--;
    }

    if (cwnd >= ssthresh) {
      avoidCongestion();
    }

    if (measuring == true && measuringSeq == header.sequence) {
      steady_clock::time_point rttEnd = steady_clock::now();

      size_t rtt = duration_cast<milliseconds>(rttEnd - rttBegin).count();
      setTimeout(false, rtt);
    }
    setTimeout(false);
    measuringSeq = header.sequence + 1;
  }

  void pollAcks(int sequence) {
    TransfererHeader header;
    ssize_t bytesReceived;

    int ret = poll(&pfd, 1, TIMEOUT_MS);

    static int lastAck = -1;
    static int AckDupCount = 0;

    if (nackedPacketsCount <= 0)
      return;

    while (ret <= 0) {
      ret = poll(&pfd, 1, TIMEOUT_MS);
    }

    while (ret > 0) {
      ret = poll(&pfd, 1, TIMEOUT_MS);
      bytesReceived = sockReceive(header);
      printf("Received %u\n", header.ackNumber);

      if (header.flags == TransferFlags::ACK) {
        if (lastAck == header.ackNumber) {
          AckDupCount++;

          if (AckDupCount >= 3) {
            tripleAcks(header.ackNumber);
            return;
          }

          doubleAcks(header.ackNumber);

          continue;
        }

        AckDupCount = 0;
        lastAck = header.ackNumber;
        acknowledgePacket(header);
      }
      ret = poll(&pfd, 1, TIMEOUT_MS);
    }
  }

  void doubleAcks(int ackNumber) {
    switch (state) {
    case SlowStart:
      break;
    case CongestionAvoidance:
      break;
    case FastRecovery:
      cwnd += 1;
      fastRecover(ackNumber);
      break;
    }
  }

  void tripleAcks(uint32_t ackNumber) {
    printf("Triple Ack\n");
    switch (state) {
    case SlowStart:
    case CongestionAvoidance:
      ssthresh = cwnd / 2;
      setCwnd(ssthresh + 3);
      fastRecover(ackNumber);
      state = FastRecovery;
      break;
    case FastRecovery:
      cwnd += 1;
      fastRecover(ackNumber);
      break;
    }
    resetNackedWindow(ackNumber);
  }

  void fastRecover(uint32_t ackNumber) {}

  void avoidCongestion() { state = CongestionAvoidance; }

  bool windowIsFull() { return nackedPacketsCount >= WINDOW_SIZE; }

  void timedOut() {
    if (nackedPacketsCount > 0 && windowIsFull())
      resetNackedWindow(measuringSeq);
  }
  void resendAllNackedPackets() {
    for (TransfererHeader &nacked : nackedPacketsBuffer) {
      sockSend(nacked);
    }
  }
  void handleFullWindow() {
    sleep_for(std::chrono::milliseconds(TIMEOUT_MS));

    // pollAcks();

    if (!windowIsFull()) {
      return;
    }

    resendAllNackedPackets();
  }

  void measureRTT(int sequence) {
    if (measuring == false) {
      rttBegin = steady_clock::now();
      measuring = true;
      measuringSeq = sequence;
    }
  }

  int getRemainingCwnd() { return WINDOW_SIZE - nackedPacketsCount; }

  void sendPacketWindow(int &sequence) {
    vector<unsigned char> buffer(PAYLOAD_SIZE);
    TransfererHeader packet;

    int window = getRemainingCwnd();
    for (int i = 0; i < window && fileRemaining > 0 && !windowIsFull(); i++) {

      int bytesRead = readFileChunk(buffer);
      fileRemaining -= bytesRead;

      if (bytesRead < 0) {
        cout << "failed to stream file: file not opened\n";
        exit(-1);
      }

      if (bytesRead <= 0 && fileRemaining <= 0) {
        return;
      }

      packet = createPacket(buffer, bytesRead, sequence, DATA);
      sendPacket(packet);

      measureRTT(sequence);

      printf("Packet sended, seq `%u`\n", sequence);

      sequence++;
    }
  }

  void resetNackedWindow(uint32_t ackNumber) {
    nackedPacketsBuffer.clear();

    sequenceNumber = ackNumber;

    file.seekg(sequenceNumber * PAYLOAD_SIZE, ios::beg);

    fileRemaining = fileSize - sequenceNumber * PAYLOAD_SIZE;

    nackedPacketsCount = 0;
  }

  int sequenceNumber = 0;
  void streamFile(const string checksum) {

    while (fileRemaining > 0 || nackedPacketsCount > 0) {
      sendPacketWindow(sequenceNumber);
      pollAcks(sequenceNumber);
    }

    TransfererHeader packet{0};
    packet.sequence = sequenceNumber;
    packet.flags = FIN | DATA;
    memccpy(packet.data, checksum.c_str(), 0, checksum.size());
    packet.data[checksum.size()] = 0;

    sockSend(packet);

    nackedPacketsBuffer.push_back(packet);
    nackedPacketsCount++;

    while (nackedPacketsCount > 0) {
      pollAcks(sequenceNumber);
      setTimeout(false);
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

State Server::state(SlowStart);
uint32_t Server::cwnd(1);
uint32_t Server::ssthresh(128);
Server *Server::instance(nullptr);

int main() {
  srand(time(nullptr));
  Server server;
  return 0;
}
