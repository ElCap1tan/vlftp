#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <linux/limits.h>
#include <signal.h>
#include <arpa/inet.h>
#include <errno.h>

#define SERVER_PORT 8080
#define DAEMON_WORKING_DIR "/tmp"
#define LOG_FILE_NAME "vlftpd.log"

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

char *get_local_time_str() {
    time_t raw_time;
    struct tm * time_info;
    char *time_str;

    time(&raw_time);
    time_info = localtime(&raw_time);
    time_str = asctime(time_info);
    time_str[strlen(time_str) - 1] = 0;

    return time_str;
}

void vlftpd_shutdown(int signum) {
    (void) signum;

    printf("--- VLFTPD SHUTTING DOWN @ %s ---\n", get_local_time_str());
    exit(EXIT_SUCCESS);
}

char *read_to_end(FILE *stream) {
    char *output = NULL;
    size_t buff_size = 0;
    char *error_txt = "vlftpd: Error reading command output\n";

    if (getdelim(&output, &buff_size, '\0', stream) == -1) {
        free(output);
        perror(error_txt);
        output = malloc((strlen(error_txt) + 1) * sizeof(char));
        strcpy(output, error_txt);
    }

    output[strlen(output) - 1] = 0; // Remove newline that is somehow contained in every output

    return output;
}

void handle_cmd(int client_fd, char *args[], uint32_t arg_count) {
    FILE *pipe;
    char *srv_cmd = args[0];
    uint32_t msg_len;
    char *ans = NULL;
    // char *err_txt = "vlftpd: Unknown command\n";
    // char *ret;

    if (strcmp("pwd", srv_cmd) == 0) {
        ans = malloc((PATH_MAX + 1) * sizeof(char));
        if (!getcwd(ans, PATH_MAX + 1)) {
            perror("vlftpd: getcwd()");
            strcpy(ans, "vlftpd: Error getting current working directory");
        }
    } else if (strcmp("dir", srv_cmd) == 0) {
        ans = "ls -a";
        if (arg_count > 1) {
            if (strcmp("directory", args[1]) == 0) { ans = "ls -a -d */"; }
            else if (strcmp("files", args[1]) == 0) { ans = "ls -a -p | grep -v /"; }
        }
        pipe = popen(ans, "r");
        ans = read_to_end(pipe);
        pclose(pipe);
    } else if (strcmp("cd", srv_cmd) == 0) {
        if (arg_count > 1) {
            if (chdir(args[1]) == 0) {
                ans = "vlftpd: SUCCESS!";
            } else {
                ans = "vlftpd: Error changing the current directory.";
            }
        } else {
            ans = "vlftpd: Error changing the current directory. Missing argument!";
        }
    } else if (strcmp("get", srv_cmd) == 0) {
        // TODO: Handle get cmd
    } else if (strcmp("put", srv_cmd) == 0) {
        // TODO: Handle put cmd
    } else {
        ans = "vlftpd: Unknown command.";
    }

    msg_len = strlen(ans) + 1;
    printf("MSG_LEN: %d\n", msg_len);
    msg_len = htonl(msg_len);

    // Write msg len
    write(client_fd, &msg_len, sizeof(msg_len));

    // Write msg
    write(client_fd, ans, strlen(ans) + 1);
    puts(ans);

    if (strcmp("pwd", srv_cmd) == 0 || strcmp("dir", srv_cmd) == 0) {
        free(ans);
    }
}

void client_fd_to_ip_str(int client_fd, char *buff) {
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    if (getpeername(client_fd, (struct sockaddr *)&addr, &addr_size) != 0) {
        strcpy(buff, "[UNKNOWN]");
    } else {
        strcpy(buff, inet_ntoa(addr.sin_addr));
    }
}

int main() {
    int server_pid;
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    int errcode;
    uint addr_length;
    uint32_t arg_count, msg_len;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("vlftpd: Error creating socket");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    errcode = bind(server_fd, (const struct sockaddr *) &server_addr, sizeof(server_addr));
    if (errcode < 0) {
        perror("vlftpd: Failed to bind socket");
        return EXIT_FAILURE;
    }

    server_pid = fork();
    if (server_pid < 0) {
        perror("vlftpd: Failed to fork a child process");
        return EXIT_FAILURE;
    } else if (server_pid != 0) { // Parent
        printf("Daemon now running on Port %d with PID %d.\n"
               "Output will be logged to %s/%s\nForeground process now exiting...\n",
               SERVER_PORT, server_pid, DAEMON_WORKING_DIR, LOG_FILE_NAME);
        return EXIT_SUCCESS;
    }

    // Daemon Process

    signal(SIGTERM, vlftpd_shutdown);
    signal(SIGKILL, vlftpd_shutdown);
    signal(SIGTERM, vlftpd_shutdown);

    chdir(DAEMON_WORKING_DIR);
    // umask(0);
    if (setsid() == -1) {
        perror("vlftpd: Failed to change session id");
        return EXIT_FAILURE;
    }

    close(STDIN_FILENO);
    freopen(LOG_FILE_NAME, "a+", stdout);
    freopen(LOG_FILE_NAME, "a+", stderr);
    setvbuf(stdout, NULL, _IOLBF, sysconf(_SC_PAGESIZE));
    setvbuf(stderr, NULL, _IOLBF, sysconf(_SC_PAGESIZE));

    printf("--- VLFTPD STARTED @ %s ---\n", get_local_time_str());
    // printf("BUF: %ld\n", sysconf(_SC_PAGESIZE));

    errcode = listen(server_fd, 5);
    if (errcode < 0) {
        perror("vlftpd: Error listening on socket");
        return EXIT_FAILURE;
    }

    while (1) {
        addr_length = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &addr_length);
        if (client_fd < 0) {
            perror("vlftpd: Failed to accept client.");
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        client_fd_to_ip_str(client_fd, client_ip);
        printf("Connected to client '%s'...\n", client_ip);

        // Read argument count
        read(client_fd, &arg_count, sizeof(arg_count));
        arg_count = ntohl(arg_count);

        char *args[arg_count];

        // Read arguments
        for (int i = 0; i < arg_count; i++) {
            read(client_fd, &msg_len, sizeof(msg_len));
            msg_len = ntohl(msg_len);

            args[i] = malloc(msg_len * sizeof(char));
            read(client_fd, args[i], msg_len);
        }
        printf("Received command '%s'...\n", args[0]);
        for (int i = 1; i < arg_count; i++) {
            printf("Argument %d: '%s'\n", i, args[i]);
        }

        handle_cmd(client_fd, args, arg_count);

        for (int i = 0; i < arg_count; i++) {
            free(args[i]);
        }

        close(client_fd);
        printf("Connection to client '%s' closed.\n", client_ip);

        /*
        puts(output);

        length = strlen(output) + 1;
        write(client_fd, &length, sizeof(length));
        write(client_fd, output, length);

        free(output);
        close(client_fd);
        */
    }

    return EXIT_SUCCESS;
}