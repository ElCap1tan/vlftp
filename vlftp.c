#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define SERVER_PORT 8080
#define MAX_ARGS 2

void print_usage(char *argv[]) {
    printf("Usage: '%s server command [argument1 [argument2]]'\n", argv[0]);
}

ssize_t read_n(int fd, void *buf, size_t n) {
    size_t	total_read = 0;
    ssize_t	currently_read = 0;

    uint8_t *b = (uint8_t* ) buf;

    while (total_read < n) {
        if ((currently_read = read(fd, b + total_read, n - total_read)) < 0 && errno != EINTR)
            continue;

        if (currently_read <= 0)
            break;

        total_read += currently_read;
    }

    return (total_read == 0 && currently_read < 0) ? currently_read : (ssize_t) total_read;
}

ssize_t write_n(int fd, const void *buf, size_t n) {
    size_t	total_written = 0;
    ssize_t	currently_written = 0;

    const uint8_t *b = (const uint8_t *) (buf);

    while (total_written < n) {
        if ((currently_written = write(fd, b + total_written, n - total_written)) < 0 && errno != EINTR)
            continue;

        if (currently_written <= 0)
            break;

        total_written += currently_written;
    }

    return (total_written == 0 && currently_written < 0) ? currently_written : (ssize_t) total_written;
}

struct sockaddr_in* resolve_hostname(char *hostname) {
    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    int errcode = getaddrinfo(hostname, NULL, &hints, &res);
    if (errcode != 0) {
        fprintf(stderr, "getaddrinfo: %s", gai_strerror(errcode));
        exit(EXIT_FAILURE);
    }

    return (struct sockaddr_in *) res->ai_addr;
}

void validate_cmd(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr,
                "You need to supply at least 2 arguments but you provided %d.\n", argc-1);
        print_usage(argv);
        exit(EXIT_FAILURE);
    }
    else if (strcmp(argv[2], "pwd") == 0 || strcmp(argv[2], "dir") == 0) { return; }
    else if (strcmp(argv[2], "cd") == 0 && argc < 4) {
        fprintf(stderr,
                "Missing directory argument for cd command.\n");
        print_usage(argv);
        exit(EXIT_FAILURE);
    } else if ((strcmp(argv[2], "get") == 0 || strcmp(argv[2], "put") == 0) && argc < 4) {
        fprintf(stderr,
                "Missing file argument for %s command.\n", argv[2]);
        print_usage(argv);
        exit(EXIT_FAILURE);
    }
}

void check_write(ssize_t written, size_t to_write) {
    if (written != to_write) {
        perror("Failed to write");
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char *argv[]) {
    char *hostname;
    char *cmd;
    struct sockaddr_in *server_addr;
    int sock_fd, errcode, to_send;
    uint32_t nbytes, nbytes_nl;
    ssize_t written;

    if (argc == 1) {
        print_usage(argv);
        return EXIT_SUCCESS;
    }

    validate_cmd(argc, argv);

    to_send = argc - 2;
    if (to_send > MAX_ARGS + 1) { to_send = MAX_ARGS + 1; }

    hostname = argv[1];

    server_addr = resolve_hostname(hostname);
    server_addr->sin_port = htons(SERVER_PORT);

    // printf("IP: %s, Port: %d, Fam: %hu\n", inet_ntoa(server_addr->sin_addr), ntohs(server_addr->sin_port), server_addr->sin_family);

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Error creating socket");
        return EXIT_FAILURE;
    }

    errcode = connect(sock_fd, (const struct sockaddr *) server_addr, sizeof(*server_addr));
    if (errcode < 0) {
        perror("Error connecting to server");
        return EXIT_FAILURE;
    }

    // Write number of arguments to transfer
    uint32_t to_send_nl = htonl(to_send);
    written = write(sock_fd, &to_send_nl, sizeof(to_send_nl));
    check_write(written, sizeof(to_send_nl));

    // Transfer arguments
    for (int i = 2; i < to_send + 2; i++) {
        nbytes = strlen(argv[i]) + 1;
        nbytes_nl = htonl(nbytes);

        // Write length of arg
        written = write(sock_fd, &nbytes_nl, sizeof(nbytes_nl));
        check_write(written, sizeof(nbytes_nl));

        // Write argument
        written = write(sock_fd, argv[i], nbytes);
        check_write(written, nbytes);
    }

    read(sock_fd, &nbytes_nl, sizeof(nbytes_nl));
    // printf("To rcv: %zu bytes\n", nbytes);
    nbytes = ntohl(nbytes_nl);
    char buff[nbytes];
    read(sock_fd, buff, sizeof(buff));
    // printf("Buff:\n%s", buff);
    puts(buff);

    close(sock_fd);

    return EXIT_SUCCESS;
}
