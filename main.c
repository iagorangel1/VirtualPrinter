#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winspool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Winspool.lib")

/*
 * Ponte de impressão RAW para Windows. O servidor só escuta em loopback;
 * use-a através de um túnel RDP/VPN configurado e autorizado. Este programa
 * não implementa o protocolo RDPDR nem criptografa o tráfego por conta própria.
 * O token também trafega no túnel: não exponha a porta diretamente à rede.
 *
 *   main.exe server <porta> <impressora> <token>
 *   main.exe client <host-do-tunel> <porta> <arquivo-raw> <token>
 */

#define MAX_TOKEN_LENGTH 4096
#define BUFFER_SIZE 16384

static int send_all(SOCKET s, const char *data, int length)
{
	int sent = 0;
	while (sent < length) {
		int n = send(s, data + sent, length - sent, 0);
		if (n <= 0) return 0;
		sent += n;
	}
	return 1;
}

static int recv_all(SOCKET s, char *data, int length)
{
	int received = 0;
	while (received < length) {
		int n = recv(s, data + received, length - received, 0);
		if (n <= 0) return 0;
		received += n;
	}
	return 1;
}

static int valid_port(const char *text, unsigned short *port)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 10);
	if (!text[0] || !end || *end || value == 0 || value > 65535) return 0;
	*port = (unsigned short)value;
	return 1;
}

static int run_server(unsigned short port, const char *printer, const char *token)
{
	SOCKET listener = INVALID_SOCKET;
	struct sockaddr_in address;
	HANDLE printer_handle = NULL;
	int result = 1;

	if (strlen(token) > MAX_TOKEN_LENGTH) {
		fprintf(stderr, "Token muito longo.\n");
		return 1;
	}
	if (!OpenPrinterA((LPSTR)printer, &printer_handle, NULL)) {
		fprintf(stderr, "Não foi possível abrir a impressora (erro %lu).\n", GetLastError());
		return 1;
	}

	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET) goto socket_error;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
		listen(listener, SOMAXCONN) == SOCKET_ERROR) goto socket_error;

	printf("Aguardando trabalhos para %s em 127.0.0.1:%u...\n", printer, port);
	for (;;) {
		SOCKET client = accept(listener, NULL, NULL);
		DWORD network_length, token_length;
		char received_token[MAX_TOKEN_LENGTH + 1];
		char buffer[BUFFER_SIZE];
		DOC_INFO_1A document;
		DWORD job_id;
		int n, page_started = 0, doc_started = 0;

		if (client == INVALID_SOCKET) continue;
		if (!recv_all(client, (char *)&network_length, sizeof(network_length))) {
			closesocket(client);
			continue;
		}
		token_length = ntohl(network_length);
		if (token_length == 0 || token_length > MAX_TOKEN_LENGTH ||
			!recv_all(client, received_token, (int)token_length)) {
			closesocket(client);
			continue;
		}
		received_token[token_length] = '\0';
		if (strlen(token) != token_length || memcmp(token, received_token, token_length) != 0) {
			fprintf(stderr, "Conexão recusada: token inválido.\n");
			closesocket(client);
			continue;
		}

		memset(&document, 0, sizeof(document));
		document.pDocName = (LPSTR)"VirtualPrinter job";
		document.pDatatype = (LPSTR)"RAW";
		job_id = StartDocPrinterA(printer_handle, 1, (LPBYTE)&document);
		if (job_id != 0 && StartPagePrinter(printer_handle)) {
			doc_started = page_started = 1;
			while ((n = recv(client, buffer, sizeof(buffer), 0)) > 0) {
				DWORD written = 0;
				if (!WritePrinter(printer_handle, buffer, (DWORD)n, &written) || written != (DWORD)n) {
					fprintf(stderr, "Falha ao escrever na impressora (erro %lu).\n", GetLastError());
					break;
				}
			}
			if (n == 0) printf("Trabalho de impressão recebido.\n");
		} else {
			fprintf(stderr, "Não foi possível iniciar o trabalho (erro %lu).\n", GetLastError());
		}
		if (page_started) EndPagePrinter(printer_handle);
		if (doc_started) EndDocPrinter(printer_handle);
		closesocket(client);
	}

socket_error:
	fprintf(stderr, "Erro de socket (%d).\n", WSAGetLastError());
	result = 1;
	if (listener != INVALID_SOCKET) closesocket(listener);
	ClosePrinter(printer_handle);
	return result;
}

static int run_client(const char *host, unsigned short port,
					  const char *path, const char *token)
{
	SOCKET connection = INVALID_SOCKET;
	struct addrinfo hints, *addresses = NULL, *it;
	FILE *input = NULL;
	char buffer[BUFFER_SIZE];
	size_t token_length = strlen(token), n;
	DWORD network_length;
	int connected = 0, result = 1;
	char service[6];

	if (token_length == 0 || token_length > MAX_TOKEN_LENGTH) {
		fprintf(stderr, "Token inválido (1-%d caracteres).\n", MAX_TOKEN_LENGTH);
		return 1;
	}
	input = fopen(path, "rb");
	if (!input) {
		perror("Não foi possível abrir o arquivo");
		return 1;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	_snprintf_s(service, sizeof(service), _TRUNCATE, "%u", port);
	if (getaddrinfo(host, service, &hints, &addresses) != 0) {
		fprintf(stderr, "Não foi possível resolver o host.\n");
		goto cleanup;
	}
	for (it = addresses; it != NULL; it = it->ai_next) {
		connection = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (connection != INVALID_SOCKET &&
			connect(connection, it->ai_addr, (int)it->ai_addrlen) == 0) {
			connected = 1;
			break;
		}
		if (connection != INVALID_SOCKET) closesocket(connection);
		connection = INVALID_SOCKET;
	}
	if (!connected) {
		fprintf(stderr, "Não foi possível conectar ao servidor.\n");
		goto cleanup;
	}

	network_length = htonl((DWORD)token_length);
	if (!send_all(connection, (const char *)&network_length, sizeof(network_length)) ||
		!send_all(connection, token, (int)token_length)) {
		fprintf(stderr, "Falha ao enviar autenticação.\n");
		goto cleanup;
	}
	while ((n = fread(buffer, 1, sizeof(buffer), input)) > 0) {
		if (!send_all(connection, buffer, (int)n)) {
			fprintf(stderr, "Falha ao enviar o trabalho.\n");
			goto cleanup;
		}
	}
	if (ferror(input)) {
		fprintf(stderr, "Erro ao ler o arquivo.\n");
		goto cleanup;
	}
	shutdown(connection, SD_SEND);
	printf("Arquivo enviado.\n");
	result = 0;

cleanup:
	if (addresses) freeaddrinfo(addresses);
	if (connection != INVALID_SOCKET) closesocket(connection);
	if (input) fclose(input);
	return result;
}

int main(int argc, char **argv)
{
	WSADATA wsa;
	unsigned short port;
	int result;

	if (argc < 2 ||
		(strcmp(argv[1], "server") == 0 && argc != 5) ||
		(strcmp(argv[1], "client") == 0 && argc != 6) ||
		(strcmp(argv[1], "server") != 0 && strcmp(argv[1], "client") != 0)) {
		fprintf(stderr,
			"Uso:\n"
			"  %s server <porta> <impressora> <token>\n"
			"  %s client <host> <porta> <arquivo-raw> <token>\n",
			argv[0], argv[0]);
		return 2;
	}

	if (!valid_port(argv[1][0] == 's' ? argv[2] : argv[3], &port)) {
		fprintf(stderr, "Porta inválida.\n");
		return 2;
	}
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "Falha ao iniciar Winsock.\n");
		return 1;
	}
	result = argv[1][0] == 's'
		? run_server(port, argv[3], argv[4])
		: run_client(argv[2], port, argv[4], argv[5]);
	WSACleanup();
	return result;
}
