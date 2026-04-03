#include <arpa/inet.h>
#include <poll.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <regex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace std;

#define PORT 8080
#define FAIL -1
#define TIMEOUT_MS 13

#define WINDOW_SIZE 100

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

class Server {

public:
  pollfd pfd;
  sockaddr_in serverAddress;
  int serverSocket;
  sockaddr_in clientAddress;
  ifstream file;
  vector<unsigned char> buffer;
  vector<TransfererHeader> nackedBuffer;
  int nackedPacketsCount;
  string ipv4;

  Server() : ipv4(""), file(), buffer(1024), nackedBuffer(1024), nackedPacketsCount(0){
    cout << "Server address: ";
    getline(cin, ipv4);
    regex pattern("^(((?!25?[6-9])[12]\\d|[1-9])?\\d\\.?\\b){4}$");

    if (!regex_search(ipv4, pattern) && !ipv4.empty())
      exit(-1);

    // creating socket
    serverSocket = socket(AF_INET, SOCK_DGRAM,
                          0); // DGRAM -> manual disse que DGRAM seria
                              // datagramas, suponho então que seja uma conexão
                              // udp, já que as outras são 'confiaveis'

    // specifying the address
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

    // binding socket.
    if (bind(serverSocket, (struct sockaddr *)&serverAddress,
             sizeof(serverAddress)) == FAIL) {
      cout << "failed to bind\n";
      exit(-1);
    }

    pfd.fd = serverSocket;
    pfd.events = POLLIN;

    char buff[1024];
    socklen_t clientLen = sizeof(clientAddress);

    ssize_t bytesReceived = recvfrom(serverSocket, buff, sizeof(buff) - 1, 0,
                                     (sockaddr *)&clientAddress, &clientLen);

    if (bytesReceived == -1) {
      cout << "failed to receive\n";
      exit(-1);
    }

    buff[bytesReceived] = '\0';

    string request(buff);

    cout << "Message from client: " << buff << '\n';

    const char *response;

    string temp;

    handleRequest(request, response);

    // closing the socket.
    close(serverSocket);
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

  /**
   * return 1 when read more than zero bytes;
   * return 0 when read less or equal zero bytes;
   * return -1 when file is null;
   * */
  int readFileChunk(struct TransfererHeader &data) {
    if (!file.is_open())
      return -1;

    file.read(reinterpret_cast<char *>(buffer.data()), buffer.size());

    std::streamsize bytesRead = file.gcount();

    memccpy(data.data, buffer.data(), 0, bytesRead);

    data.dataSize = bytesRead;

    if (bytesRead > 0) {
      data.flags = TransferFlags::DATA;
      return 1;
    }

    data.flags = TransferFlags::END;
    return 0;
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

  void handleRequest(string &request, const char *&response) {
    if (string::npos == request.find("GET")) {
      response = "Error: Bad request";
    }

    string filename = request.substr(4, request.length());

    if (!fileExistsInRoot(filename)) {
      response = "Seu GET é uma merda\n";
      return;
    }

    response = "Você fez um GET então\n";
    string checksum = sha256File(filename);

    struct TransfererHeader data;
    openFile(filename);
    streamFile(checksum);
  }
  void streamFile(const string checksum) {
    struct TransfererHeader data;

    int sequence = 0;

    while (!file.eof()) {

        int temp = nackedPacketsCount;
        struct TransfereeHeader header;
        ssize_t bytesReceived;
        socklen_t clientLen = sizeof(clientAddress);

        int ret = poll(&pfd, 1, TIMEOUT_MS);

        while (ret > 0) {
          bytesReceived =
              recvfrom(serverSocket, &header, sizeof(header), 0,
                       (sockaddr *)&clientAddress, &clientLen);

          if (header.flags == TransferFlags::ACK) {

		  bool erased = false;
            for (int i = 0; i < nackedBuffer.size(); i++) {

              if (nackedBuffer[i].sequence == header.sequence) {
                nackedBuffer.erase(nackedBuffer.begin() + i);
                nackedPacketsCount--;
		erased = true;
                break;
              }
            }
	    if (!erased && header.sequence == nackedBuffer[0].sequence - 1) {
              for (auto& nacked: nackedBuffer){
	        sendto(serverSocket, &nacked, sizeof(nacked), 0, (sockaddr *)&clientAddress,
		     sizeof(clientAddress));
	      }
	    }
          }
          ret = poll(&pfd, 1, TIMEOUT_MS);
        } 
      if (nackedPacketsCount >= WINDOW_SIZE) {
        cout << "window is full\n";
	for (auto& nacked: nackedBuffer){
		sendto(serverSocket, &nacked, sizeof(nacked), 0, (sockaddr *)&clientAddress,
				sizeof(clientAddress));
	}
	continue;
      }

      if (readFileChunk(data) == FAIL) {
        cout << "failed to stream file: file not opened\n";
        exit(-1);
      }

      data.sequence = sequence;

      sendto(serverSocket, &data, sizeof(data), 0, (sockaddr *)&clientAddress,
             sizeof(clientAddress));

      nackedBuffer.push_back(data);
      nackedPacketsCount++;

      sequence++;
    }

    readFileChunk(data);

    data.sequence = sequence;
    memccpy(data.data, checksum.c_str(), 0, checksum.size());
    data.data[checksum.size()] = 0;
    sendto(serverSocket, &data, sizeof(data), 0, (sockaddr *)&clientAddress,
           sizeof(clientAddress));

    nackedBuffer.push_back(data);
    nackedPacketsCount++;

    while (nackedPacketsCount > 0) {
      struct TransfereeHeader header;

      socklen_t clientLen = sizeof(clientAddress);
      ssize_t bytesReceived = recvfrom(serverSocket, &header, sizeof(header), 0,
                                       (sockaddr *)&clientAddress, &clientLen);

      if (header.flags == TransferFlags::ACK) {

        for (int i = 0; i < nackedBuffer.size(); i++) {

          if (nackedBuffer[i].sequence == header.sequence) {
            nackedBuffer.erase(nackedBuffer.begin() + i);
            nackedPacketsCount--;
            break;
          }
        }
      }
    }
  }
};
int main() {
  Server server;
  return 0;
}
