#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#define SERVER_PORT 8080
#define MAX_ARGS 2 // Maximal number of arguments that are needed for any given command and thus send to the server.

void print_usage(char *argv[]) {
    printf("Usage: '%s server command [argument1 [argument2]]'\n", argv[0]);
}

// Wrapper around the read() method to handle partial reads. Reads exactly n bytes if possible.
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

// Wrapper around the write() method to handle partial writes. Writes exactly n bytes if possible.
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

void check_write(ssize_t written, size_t to_write) {
    if (written != to_write) {
        perror("Failed to write");
        exit(EXIT_FAILURE);
    }
}

// Resolves a hostname string to the corresponding IP address.
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

// Very liberally check arguments for validity to stop the client from sending commands that can't possibly be
// fulfilled to the server. Only checks the number of provided arguments. If not enough arguments for the given command
// are provided, exit with failure. Further error handling and reporting is left to the server.
void validate_args(int argc, char *argv[]) {
    if (argc == 1) {
        print_usage(argv);
        exit(EXIT_SUCCESS);
    }

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

// Reads a file from disk and saves the amount of bytes read in the long pointed to by f_size.
char *read_file(FILE *f, long *f_size) {
    char *string;

    fseek(f, 0, SEEK_END);
    *f_size = ftell(f);
    // printf("Filesize: %ld\n", *f_size);
    rewind(f);

    string = malloc(*f_size + 1);
    fread(string, *f_size, 1, f);

    string[*f_size] = 0;

    return string;
}

int main(int argc, char *argv[]) {
    char *hostname;
    char *cmd;
    char *buf;
    long *file_length;
    struct sockaddr_in *server_addr;
    int sock_fd, errcode, to_send;
    uint32_t msg_len, msg_len_nl;
    ssize_t written;
    uint16_t success; // Used as bool for 'get' command. 1 if the file transfer was successful and 0 otherwise.

    validate_args(argc, argv);

    to_send = argc - 2;
    if (to_send > MAX_ARGS + 1) { to_send = MAX_ARGS + 1; }

    hostname = argv[1];
    cmd = argv[2];

    server_addr = resolve_hostname(hostname);
    server_addr->sin_port = htons(SERVER_PORT);

    // printf("IP: %s, Port: %d, Fam: %hu\n", inet_ntoa(server_addr->sin_addr), ntohs(server_addr->sin_port), server_addr->sin_family);

    if (strcmp(cmd, "put") == 0) {
        FILE *file = fopen(argv[3], "r");
        if (file) {
            file_length = malloc(sizeof(long));
            buf = read_file(file, file_length);
            fclose(file);
        } else {
            perror("The file requested can't be transferred");
            return EXIT_FAILURE;
        }
    }

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
    written = write_n(sock_fd, &to_send_nl, sizeof(to_send_nl));
    check_write(written, sizeof(to_send_nl));

    // Transfer arguments
    for (int i = 2; i < to_send + 2; i++) {
        msg_len = strlen(argv[i]) + 1;
        msg_len_nl = htonl(msg_len);

        // Write length of arg
        written = write_n(sock_fd, &msg_len_nl, sizeof(msg_len_nl));
        check_write(written, sizeof(msg_len_nl));

        // Write argument
        written = write_n(sock_fd, argv[i], msg_len);
        check_write(written, msg_len);
    }

    if (strcmp(cmd, "put") == 0) {
        msg_len = *file_length;
        free(file_length);

        msg_len_nl = htonl(msg_len);
        written = write_n(sock_fd, &msg_len_nl, sizeof(msg_len_nl));
        check_write(written, sizeof(msg_len_nl));

        written = write_n(sock_fd, buf, msg_len);
        check_write(written, msg_len);
        free(buf);
    } else if (strcmp(cmd, "get") == 0) {
        read_n(sock_fd, &success, sizeof(success));
    }

    read_n(sock_fd, &msg_len_nl, sizeof(msg_len_nl));
    // printf("To rcv: %zu bytes\n", msg_len);
    msg_len = ntohl(msg_len_nl);
    buf = malloc(msg_len * sizeof(char));
    read_n(sock_fd, buf, msg_len);
    // printf("Buf:\n%s", buf);

    if (strcmp(cmd, "get") == 0) {
        if (success) { // If file transfer was successful save returned data to file...
            int fd;

            if (argc >= 5) fd = open(argv[4] ,O_WRONLY | O_CREAT, 0644);
            else fd = open(argv[3] ,O_WRONLY | O_CREAT, 0664);
            if (fd == -1) {
                perror("open()");
                exit(-1);
            }

            write_n(fd, buf, msg_len);

            close(fd);
        } else { // Otherwise an error message was send back, and we simply print it to the terminal.
            puts(buf);
        }
    } else {
        puts(buf);
    }

    free(buf);
    close(sock_fd);

    return EXIT_SUCCESS;
}
