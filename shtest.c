#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h> /* See NOTES */
#include <sys/wait.h>
#include <sys/socket.h>

/*------------------------------------------
    Shellcode testing program
    Usage:
        shtest [-s socked_fd_no] {-f file | $'\xeb\xfe' | '\xb8\x39\x05\x00\x00\xc3'}
    Usage example:
        $ shtest $'\xeb\xfe'                 # raw shellcode
        $ shtest '\xb8\x39\x05\x00\x00\xc3'  # escaped shellcode
        $ shtest -f test.sc                  # shellcode from file
        $ shtest -f <(python gen_payload.py) # test generated payload
        $ shtest -s 5 -f test.sc             # create socket at fd=5
            # Allows to test staged shellcodes
            # Flow is redirected like this: STDIN -> SOCKET -> STDOUT
    Compiling:
        gcc -Wall shtest.c -o shtest
    Author: hellman (hellman1908@gmail.com)
-------------------------------------------*/

char buf[4096];
int pid1, pid2;
int sock;
int ready;

void usage(char * err);
int main(int argc, char **argv);

void load_from_file(char *fname);
void copy_from_argument(char *arg);
void escape_error();

int create_sock();
void run_reader(int);
void run_writer(int);
void set_ready(int sig);

void run_shellcode(void *sc_ptr);


void usage(char * err) {
    printf("    Shellcode testing program\n\
    Usage:\n\
        shtest {-f file | $'\\xeb\\xfe' | '\\xb8\\x39\\x05\\x00\\x00\\xc3'}\n\
    Usage example:\n\
        $ shtest $'\\xeb\\xfe'                 # raw shellcode\n\
        $ shtest '\\xb8\\x39\\x05\\x00\\x00\\xc3'  # escaped shellcode\n\
        $ shtest -f test.sc                  # shellcode from file\n\
        $ shtest -f <(python gen_payload.py) # test generated payload\n\
        $ shtest -s 5 -f test.sc             # create socket at fd=5 (STDIN <- SOCKET -> STDOUT)\n\
            # Allows to test staged shellcodes\
            # Flow is redirected like this: STDIN -> SOCKET -> STDOUT\
    Compiling:\n\
        gcc -Wall shtest.c -o shtest\n\
    Author: hellman (hellman1908@gmail.com)\n");
    if (err) printf("\nerr: %s\n", err);
    exit(1);
}

int main(int argc, char **argv) {
    char * fname = NULL;
    int c;

    pid1 = pid2 = -1;
    sock = -1;

    while ((c = getopt(argc, argv, "hus:f:")) != -1) {
        switch (c) {
            case 'f':
                fname = optarg;
                break;
            case 's':
                sock = atoi(optarg);
                if (sock <= 2 || sock > 1024)
                    usage("bad descriptor number for sock");
                break;
            case 'h':
            case 'u':
                usage(NULL);
            default:
                usage("unknown argument");
        }
    }

    if (argc == 1)
        usage(NULL);

    if (optind < argc && fname)
        usage("can't load shellcode both from argument and file");
    
    if (!(optind < argc) && !fname)
        usage("please provide shellcode via either argument or file");

    if (optind < argc) {
        copy_from_argument(argv[optind]);
    }
    else {
        load_from_file(fname);
    }

    //create socket if needed
    if (sock != -1) {
        int created_sock = create_sock(sock);
        printf("Created socket %d\n", created_sock);
    }

    run_shellcode(buf);
    return 100;
}

void load_from_file(char *fname) {
    FILE * fd = fopen(fname, "r");
    if (!fd) {
        perror("fopen");
        exit(100);
    }

    int c = fread(buf, 1, 4096, fd);
    printf("Read %d bytes from '%s'\n", c, fname);
    fclose(fd);
}

void copy_from_argument(char *arg) {
    //try to translate from escapes ( \xc3 )

    bzero(buf, sizeof(buf));
    strncpy(buf, arg, sizeof(buf));

    int i;
    char *p1 = buf;
    char *p2 = buf;
    char *end = p1 + strlen(p1);

    while (p1 < end) {
        i = sscanf(p1, "\\x%02x", (unsigned int *)p2);
        if (i != 1) {
            if (p2 == p1) break;
            else escape_error();
        }

        p1 += 4;
        p2 += 1;
    }
}

void escape_error() {
    printf("Shellcode is incorrectly escaped!\n");
    exit(1);
}

int create_sock() {
    int fds[2];
    int sock2;
        
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    if (result == -1) {
        perror("socket");
        exit(101);
    }

    if (sock == fds[0]) {
        sock2 = fds[1];
    }
    else if (sock == fds[1]) {
        sock2 = fds[0];
    }
    else {
        dup2(fds[0], sock);
        close(fds[0]);
        sock2 = fds[1];
    }

    ready = 0;
    signal(SIGUSR1, set_ready);

    /*
    writer: stdin -> socket (when SC exits/fails, receives SIGCHLD and exits)
    \--> main: shellcode (when exits/fails, sends SIGCHLD to writer and closes socket)
         \--> reader: sock -> stdout (when SC exits/fails, socket is closed and reader exits)

    main saves pid1 = reader,
               pid2 = writer
    to send them SIGUSR1 right before running shellcode
    */

    pid1 = fork();
    if (pid1 == 0) {
        close(sock);
        run_reader(sock2);
    }

    pid2 = fork();
    if (pid2 > 0) { // parent - writer
        signal(SIGCHLD, exit);
        close(sock);
        run_writer(sock2);
    }
    pid2 = getppid();

    close(sock2);
    return sock;
}

void run_reader(int fd) {
    char buf[4096];
    int n;

    while (!ready) {
        usleep(0.1);
    }

    while (1) {
        n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            printf("RECV %d bytes FROM SOCKET: ", n);
            fflush(stdout);
            write(1, buf, n);
        }
        else {
            exit(0);
        }
    }
}

void run_writer(int fd) {
    char buf[4096];
    int n;
    
    while (!ready) {
        usleep(0.1);
    }

    while (1) {
        n = read(0, buf, sizeof(buf));
        if (n > 0) {
            printf("SENT %d bytes TO SOCKET\n", n);
            write(fd, buf, n);
        }
        else {
            shutdown(fd, SHUT_WR);
            close(fd);
            wait(&n);
            exit(0);
        }
    }
}

void set_ready(int sig) {
    ready = 1;
}

void run_shellcode(void *sc_ptr) {
    int ret = 0, status = 0;
    int (*ptr)();
    
    ptr = sc_ptr;
    mprotect((void *) ((unsigned int)ptr & 0xfffff000), 4096 * 2, 7);
    
    void *esp, *ebp;
    void *edi, *esi;

    asm ("movl %%esp, %0;"
         "movl %%ebp, %1;"
         :"=r"(esp), "=r"(ebp));
    
    asm ("movl %%esi, %0;"
         "movl %%edi, %1;"
         :"=r"(esi), "=r"(edi)); 
    
    printf("Shellcode at %p\n", ptr);
    printf("Registers before call:\n");
    printf("  esp: %p, ebp: %p\n", esp, ebp);
    printf("  esi: %p, edi: %p\n", esi, edi);

    printf("----------------------\n");
    if (pid1 > 0) kill(pid1, SIGUSR1);
    if (pid2 > 0) kill(pid2, SIGUSR1);

    ret = (*ptr)();

    if (sock != -1)
        close(sock);
    
    wait(&status);

    printf("----------------------\n");
    
    printf("Shellcode returned %d\n", ret);
    exit(0);
}