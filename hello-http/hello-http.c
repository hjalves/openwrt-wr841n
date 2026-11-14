#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Length: 12\r\n"
    "Connection: close\r\n"
    "\r\n"
    "Hello World\n";

static void handle_client(int client)
{
    char request[1024];
    struct timeval timeout = {3, 0};
    size_t sent = 0;
    ssize_t n;

    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    do {
        n = recv(client, request, sizeof(request), 0);
    } while (n < 0 && errno == EINTR);
    if (n <= 0)
        return;

    while (sent < sizeof(response) - 1) {
        n = send(client, response + sent, sizeof(response) - 1 - sent, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
}

int main(int argc, char **argv)
{
    struct sockaddr_in address;
    int listener, reuse = 1;
    long port = 8080;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        char *end;
        errno = 0;
        port = strtol(argv[1], &end, 10);
        if (errno || *argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return 1;
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 8) < 0) {
        perror("bind/listen");
        close(listener);
        return 1;
    }
    fprintf(stderr, "Listening on 0.0.0.0:%ld\n", port);

    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            close(listener);
            return 1;
        }
        handle_client(client);
        close(client);
    }
}
