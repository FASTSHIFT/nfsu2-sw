/* Virtual keyboard: after argv[1] s, taps W, A, ENTER, SPACE, ESC (50 ms down each) */
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
static void emit(int fd, int type, int code, int val) { struct input_event e; memset(&e, 0, sizeof e); e.type = type; e.code = code; e.value = val; write(fd, &e, sizeof e); }
int main(int argc, char **argv) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); return 1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_EVBIT, EV_SYN);
    int keys[] = { KEY_W, KEY_A, KEY_ENTER, KEY_SPACE, KEY_ESC };
    for (int i = 0; i < 5; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);
    struct uinput_user_dev u; memset(&u, 0, sizeof u); strcpy(u.name, "recomp-inject-kbd"); u.id.bustype = BUS_USB; u.id.vendor = 1; u.id.product = 2;
    write(fd, &u, sizeof u); ioctl(fd, UI_DEV_CREATE);
    sleep(argc > 1 ? atoi(argv[1]) : 6);
    for (int i = 0; i < 5; i++) { emit(fd, EV_KEY, keys[i], 1); emit(fd, EV_SYN, SYN_REPORT, 0); usleep(80000); emit(fd, EV_KEY, keys[i], 0); emit(fd, EV_SYN, SYN_REPORT, 0); usleep(300000); }
    sleep(1); ioctl(fd, UI_DEV_DESTROY); close(fd); return 0;
}
