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
#include <fcntl.h>

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

    const uint8_t *b = (const uint8_t *) buf;

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

char *read_to_null_term(FILE *stream) {
    char *output = NULL;
    size_t buf_size = 0;
    char *error_txt = "vlftpd: Error reading command output\n";

    if (getdelim(&output, &buf_size, '\0', stream) == -1) {
        free(output);
        perror(error_txt);
        output = malloc((strlen(error_txt) + 1) * sizeof(char));
        strcpy(output, error_txt);
    }

    output[strlen(output) - 1] = 0; // Remove newline that is somehow contained in every output

    return output;
}

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

char *strerror_format(int err_num, char* format_str) {
    char *errno_str = strerror(err_num);
    char *buf = malloc((strlen(format_str) + strlen(errno_str) + 1) * sizeof(char));
    sprintf(buf, format_str, errno_str);

    return buf;
}

void handle_cmd(int client_fd, char *args[], uint32_t arg_count) {
    FILE *pipe;
    char *srv_cmd = args[0];
    uint32_t msg_len, msg_len_nl;
    char *buf = NULL;
    ssize_t written;
    int free_buf = 0;
    long *file_length = NULL;

    if (strcmp("pwd", srv_cmd) == 0) {
        buf = malloc((PATH_MAX + 1) * sizeof(char));
        free_buf = 1;
        if (!getcwd(buf, PATH_MAX + 1)) {
            perror("vlftpd: getcwd()");
            buf = strerror_format(errno, "vlftpd: Error getting current working directory: %s");
        }
    } else if (strcmp("dir", srv_cmd) == 0) {
        buf = "ls -a";
        if (arg_count > 1) {
            if (strcmp("directory", args[1]) == 0) { buf = "ls -a -d */"; }
            else if (strcmp("files", args[1]) == 0) { buf = "ls -a -p | grep -v /"; }
        }
        if ((pipe = popen(buf, "r"))) {
            buf = read_to_null_term(pipe);
            pclose(pipe);
        } else {
            perror("vlftpd: popen()");
            buf = strerror_format(errno, "vlftpd: Error running the required command: %s");
        }

        free_buf = 1;
    } else if (strcmp("cd", srv_cmd) == 0) {
        if (arg_count > 1) {
            if (chdir(args[1]) == 0) {
                buf = "vlftpd: SUCCESS!";
            } else {
                buf = strerror_format(errno, "vlftpd: Error changing the current directory.");
                free_buf = 1;
            }
        } else {
            buf = "vlftpd: Error changing the current directory. Missing argument!";
        }
    } else if (strcmp("get", srv_cmd) == 0) {
        uint16_t success;

        if (arg_count > 1) {
            FILE *file = fopen(args[1], "r");
            if (file) {
                file_length = malloc(sizeof(long));
                buf = read_file(file, file_length);
                fclose(file);
                success = htons(1);
                written = write_n(client_fd, &success, sizeof(success));
                check_write(written, sizeof(success));
            } else {
                perror("vlftpd: fopen()");
                buf = strerror_format(errno, "vlftpd: Error reading the file: %s");
                success = htons(0);
                written = write_n(client_fd, &success, sizeof(success));
                check_write(written, sizeof(success));
            }

            free_buf = 1;
        } // TODO: Send error msg
    } else if (strcmp("put", srv_cmd) == 0) {
        read_n(client_fd, &msg_len_nl, sizeof(msg_len_nl));
        msg_len = ntohl(msg_len_nl);
        printf("DATA_LEN: %d\n", msg_len);

        buf = malloc(msg_len * sizeof(char));
        if (!buf) {
            perror("malloc buff");
        }
        read_n(client_fd, buf, msg_len);
        printf("DATA: \n%s\n", buf);

        int fd;
        if (arg_count > 2) {
            fd = open(args[2] ,O_WRONLY | O_CREAT, 0644);
        } else if (arg_count > 1) {
            fd = open(args[1] ,O_WRONLY | O_CREAT, 0664);
        } else {
            // TODO: Send error msg
            return;
        }

        if (fd == -1) {
            free(buf);
            buf = strerror_format(errno, "vlftpd: Error opening file: %s");
            perror("vlftpd: Error Opening file");

            free_buf = 1;
        } else {
            written = write_n(fd, buf, msg_len);
            free(buf);

            if (written != msg_len) {
                buf = strerror_format(errno, "vlftpd: Error writing file: %s");
                perror("vlftpd: Error writing to file");

                free_buf = 1;
            } else {
                buf = "vlftpd: SUCCESS!";
            }

            close(fd);
        }
    } else {
        buf = "vlftpd: Unknown command.";
    }

    if (file_length) {
        msg_len = *file_length;
        free(file_length);
    } else {
        msg_len = strlen(buf) + 1;
    }
    printf("MSG_LEN: %d\n", msg_len);
    msg_len_nl = htonl(msg_len);

    // Write msg len
    written = write_n(client_fd, &msg_len_nl, sizeof(msg_len_nl));
    check_write(written, sizeof(msg_len_nl));

    // Write msg
    written = write_n(client_fd, buf, msg_len);
    printf("Written: %zd\n", written);
    check_write(written, msg_len);
    puts(buf);

    if (free_buf) {
        free(buf);
    }
}

void client_fd_to_ip_str(int client_fd, char *buf) {
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    if (getpeername(client_fd, (struct sockaddr *)&addr, &addr_size) != 0) {
        strcpy(buf, "[UNKNOWN]");
    } else {
        strcpy(buf, inet_ntoa(addr.sin_addr));
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
        read_n(client_fd, &arg_count, sizeof(arg_count));
        arg_count = ntohl(arg_count);

        char *args[arg_count];

        // Read arguments
        for (int i = 0; i < arg_count; i++) {
            read_n(client_fd, &msg_len, sizeof(msg_len));
            msg_len = ntohl(msg_len);

            args[i] = malloc(msg_len * sizeof(char));
            read_n(client_fd, args[i], msg_len);
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
    }

    return EXIT_SUCCESS;
}