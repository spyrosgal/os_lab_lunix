#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include "lunix-chrdev.h"

int main(){
    int fd = open("/dev/lunix0-temp", O_RDONLY);
    ioctl(fd, LUNIX_IOC_MODE, CHRDEV_MODE_RAW);
    while(1) {
        char t[11];
        if(read(fd, t, 5) < 0) {
            printf("Something unexpected happened\n");
        }
        printf("%s", t);
    }
    return 0;
}