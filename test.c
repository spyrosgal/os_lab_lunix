#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include "lunix-chrdev.h"

int main(int argc, char *argv[]){
    if(argc == 1) {
        printf("Usage: ./test [TEST] [FILE] [OPTIONS]\n");
        return 0;
    }

    int fd = open((argv[2]), O_RDONLY);
    if(!strcmp(argv[1], "ioctl")) {
        if(!strcmp(argv[3], "RAW")) {
            ioctl(fd, LUNIX_IOC_MODE, CHRDEV_MODE_RAW);
            while(1) {
                char t[3];
                int tempres = read(fd, t, 2);
                if(tempres < 0) {
                    printf("Something unexpected happened\n");
                }else {
                    int d = 0;
                    d = ((int)(t[0]) << 8) + (int)t[1];
                    printf("%d\n", d);
                }
            }
        }else if(!strcmp(argv[3], "COOKED")) {
            ioctl(fd, LUNIX_IOC_MODE, CHRDEV_MODE_COOKED);
            while(1) {
                char t[11];
                int tempres = read(fd, t, 10);
                if(tempres < 0) {
                    printf("Something unexpected happened\n");
                }else if(tempres) {
                    for(int i = 0; i < tempres; i++) printf("%c", *(t+i));
                    printf("\n");
                }
            }
        }
    }else if(!strcmp(argv[1], "fork")) {
        pid_t p[5];
        for(int i = 0; i < 5; i++) {
            p[i] = fork();
            if(!p[i]) {
                while(1) {
                    char t[11];
                    int tempres = read(fd, t, 2);
                    if(tempres < 0) {
                        printf("Something unexpected happened\n");
                    }else if(tempres) {
                        for(int i = 0; i < tempres; i++) printf("%c", *(t+i));
                        printf("\n");
                    }
                }
            }
        }

        sleep(10);
        for(int i = 0; i < 5; i++) {
            kill(p[i], SIGKILL);
        }
    }
    
    return 0;
}