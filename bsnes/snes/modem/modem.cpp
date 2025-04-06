#ifdef _WIN32
#include <winsock2.h>
#endif

#include <snes.hpp>
#include <sys/types.h>
#if !defined(PLATFORM_WIN)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
int platform_startup(){return 0;}
#else
#include<windows.h>
#include <ws2tcpip.h>
static int inet_aton(const char* str, struct in_addr* adr){
	adr->s_addr = inet_addr(str);
	fprintf(stderr, "%08x %s\n", adr->s_addr, inet_ntoa(*adr));
	return 1;
}

static WSADATA wsaData;

static int platform_startup(){
	AllocConsole();
	FILE *stream;
	freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);

	int iResult;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed: %d\n", iResult);
		return 1;
	}

	return 0;	
}

#define close(x) closesocket(x)
#endif
#include <fcntl.h>
#include <errno.h>

static void socket_perror(const char* msg){
	#if !defined(PLATFORM_WIN)
		perror(msg);
	#else
		char *sys_mes = NULL;
		FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, WSAGetLastError(), 0, (LPTSTR)&sys_mes, 0, NULL);
		fprintf(stderr, "%s: 0x%x %s\n", msg, WSAGetLastError(), sys_mes);
		// MessageBox(0,sys_mes,0,0);
		LocalFree(sys_mes);
	#endif
}

#define MODEM_CPP
namespace SNES {

Modem::Modem()
{
	lbuf_pos = 0;
	abuf_head = abuf_tail = 0;
	mode = Command;
	echo_on = false;
	socketfd = -1;
	platform_startup();
}

void Modem::hangup(void)
{
	mode = Command;
	if (socketfd != -1) {
		close(socketfd);
		socketfd = -1;
	}
}

bool Modem::carrierDetect(void)
{
	return mode == Connected;
}

bool Modem::hasData(void)
{
	uint8 d;
	int res;

	if (socketfd != -1) {
		res = recv(socketfd, (char*)&d, 1, 0);
		if (res < 0) {
			if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
				socket_perror("recv");
				close(socketfd);
				socketfd = -1;
				mode = Command;
				answerCommand("\r\nNO CARRIER\r\n");
				return false;
			}
		}
		else if (res == 1) {
			addOutByte(d);
		}
	}

	if (answer_delay) {
		answer_delay--;
		return false;
	}
	return abuf_head != abuf_tail;
}

uint8 Modem::readData(void)
{
	uint8 b = 0xff;

	if (hasData()) {
      b = abuf[abuf_tail];
	  abuf_tail++;
	  if (abuf_tail >= MODEM_LINEBUF_SIZE) {
		  abuf_tail = 0;
	  }
	}
	return b;
}

void Modem::writeData(uint8 data)
{
  switch (mode) {
    case Command:
      if (lbuf_pos >= MODEM_LINEBUF_SIZE) {
        printf("modem rx buffer full\n");
        return;
      }

	  if (echo_on) {
		  if (data != '\n')
			  addOutByte(data);
	  }

      lbuf[lbuf_pos] = data;
	  if (data == 0x0d)
		  return;

      if (data == 0x0a) {
		lbuf[lbuf_pos] = 0;
	    processCommandBuffer();
		lbuf_pos = 0;
      } else {
		lbuf_pos++;
	  }
	  break;

    case Connected:
	  printf("%02x ", data); fflush(stdout);
	  if (socketfd >= 0) {
		  if (send(socketfd, (char*)&data, 1, 0)<0) {
			  socket_perror("send");
		  }
	  }
	  break;
  }
}

bool Modem::canWrite()
{
	return lbuf_pos < MODEM_LINEBUF_SIZE;
}

void Modem::processCommandBuffer(void)
{
	const char *MsgOK = "\r\nOK\r\n";
	const char *MsgERROR = "\r\nERROR\r\n";
	int local_socketfd = -1;

	if (strncmp((char*)lbuf, "AT", 2)) {
		printf("???: \"%s\"\n", (char*)lbuf);
		return;
	}

	printf("AT command: \"%s\"\n", (char*)lbuf);

	if (strcmp((char*)lbuf, "ATE1Q0V1")==0) {
		echo_on = true;
    	answerCommand(MsgOK);
		return;
	}

	if (strcmp((char*)lbuf, "AT&F&W0&W1")==0) {
    	answerCommand(MsgOK);
		return;
	}

	if (strcmp((char*)lbuf, "ATZ")==0) {
    	answerCommand(MsgOK);
		return;
	}

	// ATP : Set pulse dialing
	if (strncmp((char*)lbuf, "ATP", 3)==0) {
		// note: JRA PAT sends ATP&P1 or ATP&P2 depending on line classification.
		// &P1: 100pps
		// &P2: 200pps
    	answerCommand(MsgOK);
		return;
	}

	// ATT : Set tone dialing
	if (strncmp((char*)lbuf, "ATT", 3)==0) {
    	answerCommand(MsgOK);
		return;
	}

	// ATS: Set S registers
	if (strncmp((char*)lbuf, "ATS", 3)==0) {
		int reg, value;
		if (2 == sscanf((char*)lbuf, "ATS%d=%d", &reg, &value)) {
			printf("Set S register %d to %d\n", reg, value);
    		answerCommand(MsgOK);
		} else {
			answerCommand(MsgERROR);
		}
		return;
	}

	// ATL: Set speaker volume (0-3)
	if (strncmp((char*)lbuf, "ATL", 3)==0) {
    	answerCommand(MsgOK);
		return;
	}

	// Error
	if (strncmp((char*)lbuf, "ATX", 3)==0) {
		if (1 == sscanf((char*)lbuf, "ATX%d", &atx)) {
    		answerCommand(MsgOK);
		} else {
    		answerCommand(MsgERROR);
		}
		return;
	}

	// Proprietary command for choosing connection rate?
	if (strncmp((char*)lbuf, "AT%B", 4)==0) {
		if (1==sscanf((char*)lbuf, "AT%%B%d", &connection_rate)) {
			printf("Requesting connection baud rate %d\n", connection_rate);
    		answerCommand(MsgOK);
		} else {
			answerCommand(MsgERROR);
		}
		return;
	}

	if (strcmp((char*)lbuf, "AT\\N0%C0")==0) {
    	answerCommand(MsgOK);
		return;
	}

	if (strcmp((char*)lbuf, "AT\\N3%C0")==0) {
    	answerCommand(MsgOK);
		return;
	}

	if (strcmp((char*)lbuf, "AT\\N3%C1")==0) {
    	answerCommand(MsgOK);
		return;
	}

	// Dial
	if (strncmp((char*)lbuf, "ATD", 3)==0) {
		char connectStr[32];
		int res, flags;
		struct sockaddr_in destination;

		memset(&destination, 0, sizeof(struct sockaddr_in));
		destination.sin_family = AF_INET;
		destination.sin_port = htons(5555);
		inet_aton("127.0.0.1", &destination.sin_addr);

		local_socketfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (local_socketfd == -1) {
			socket_perror("socket");
			answerCommand("\r\nBUSY\r\n");
			return;
		}

		res = connect(local_socketfd, (struct sockaddr *)&destination, sizeof(struct sockaddr));
		if (res == -1) {
			socket_perror("connect");
			close(local_socketfd);
			local_socketfd = -1;
			answerCommand("\r\nBUSY\r\n");
			return;
		}

#if _WIN32
		u_long val=1;
		ioctlsocket(local_socketfd, FIONBIO, &val);
#else
		flags = fcntl(local_socketfd, F_GETFL, 0);
		if (flags == -1) {
			socket_perror("fcntl");
			close(local_socketfd);
			local_socketfd = -1;
			answerCommand("\r\nBUSY\r\n");
			return;
		}

		flags |= O_NONBLOCK;

		if (fcntl(local_socketfd, F_SETFL, flags)) {
			socket_perror("fcntl");
			close(local_socketfd);
			local_socketfd = -1;
			answerCommand("\r\nBUSY\r\n");
			return;
		}
#endif

		snprintf(connectStr, 32, "\r\nCONNECT %d\r\n", connection_rate);
		answerCommand(connectStr);
		mode = Connected;
		socketfd = local_socketfd;
		return;
	}

}

void Modem::addOutByte(uint8 dat)
{
	abuf[abuf_head] = dat;
	abuf_head++;
	if (abuf_head >= MODEM_LINEBUF_SIZE)
		abuf_head = 0;

	answer_delay = 1;
}

void Modem::answerCommand(const char *str)
{
	printf("Reply: ", str);
	while (*str) {
		if (*str >= 32) {
			printf("%c", *str);
		} else {
			printf("<%02x>", *str);
		}

		addOutByte(*str);
		str++;
	}
	printf("\n");
}

}
