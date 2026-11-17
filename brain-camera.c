#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void stopped(int sig) { (void)sig; stop = 1; }
static int xioctl(int fd, unsigned long cmd, void *arg) {
    int ret;
    do { ret = ioctl(fd, cmd, arg); } while (ret < 0 && errno == EINTR && !stop);
    return ret;
}
static unsigned clamp(int x) { return x < 0 ? 0 : x > 255 ? 255 : (unsigned)x; }
static uint16_t pixel(unsigned y, int u, int v, const struct fb_var_screeninfo *f) {
    int c = (int)y - 16;
    unsigned r = clamp((298*c + 409*v + 128) >> 8);
    unsigned g = clamp((298*c - 100*u - 208*v + 128) >> 8);
    unsigned b = clamp((298*c + 516*u + 128) >> 8);
    return (uint16_t)(((r >> (8-f->red.length)) << f->red.offset) |
                      ((g >> (8-f->green.length)) << f->green.offset) |
                      ((b >> (8-f->blue.length)) << f->blue.offset));
}
struct buffer { void *ptr; size_t len; };

int main(int argc, char **argv) {
    const char *device = "/dev/video0";
    unsigned frames = 0, fps = 10, count = 0;
    int stretch = 0, no_display = 0, small = 0, status = 1;
    int video = -1, fb = -1, tty = -1, tty_mode = -1, raw = 0, streaming = 0;
    struct termios original;
    struct buffer bufs[4] = {{0}};
    unsigned nbuf = 0;
    uint8_t *screen = MAP_FAILED;
    size_t screen_len = 0;
    uint16_t *row = NULL;
    struct fb_fix_screeninfo fix = {0};
    struct fb_var_screeninfo var = {0};
    for (int i=1; i<argc; ++i) {
        if (!strcmp(argv[i], "--stretch")) stretch = 1;
        else if (!strcmp(argv[i], "--small")) small = 1;
        else if (!strcmp(argv[i], "--no-display")) no_display = 1;
        else if (!strcmp(argv[i], "--device") && i+1<argc) device = argv[++i];
        else if ((!strcmp(argv[i], "--frames") || !strcmp(argv[i], "--fps")) && i+1<argc) {
            int is_frames = !strcmp(argv[i], "--frames");
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || value > 1000000 || (!is_frames && (value < 1 || value > 30))) {
                fprintf(stderr, "Invalid numeric option\n"); return 2;
            }
            if (is_frames) frames = (unsigned)value; else fps = (unsigned)value;
        } else {
            fprintf(stderr, "Usage: %s [--stretch|--small] [--fps 1..30] [--device /dev/video0] [--frames N] [--no-display]\n", argv[0]);
            return 2;
        }
    }
    struct sigaction sa = {0};
    sa.sa_handler = stopped;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
    video = open(device, O_RDWR | O_NONBLOCK);
    if (video < 0) { perror(device); goto done; }
    struct v4l2_capability cap = {0};
    if (xioctl(video, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); goto done; }
    unsigned caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming video capture\n"); goto done;
    }
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 320; fmt.fmt.pix.height = 240;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(video, VIDIOC_S_FMT, &fmt) < 0) { perror("VIDIOC_S_FMT"); goto done; }
    unsigned width = fmt.fmt.pix.width, height = fmt.fmt.pix.height, stride = fmt.fmt.pix.bytesperline;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV || !width || !height || (width & 1) || width > 4096 || height > 4096 || stride < width*2) {
        fprintf(stderr, "Unsupported capture format\n"); goto done;
    }
    struct v4l2_streamparm parm = {0};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (xioctl(video, VIDIOC_S_PARM, &parm) < 0) perror("VIDIOC_S_PARM (continuing)");
    unsigned dw = width, dh = height, left = 0, top = 0;
    if (!no_display) {
        fb = open("/dev/fb0", O_RDWR);
        if (fb < 0) { perror("/dev/fb0"); goto done; }
        if (ioctl(fb, FBIOGET_FSCREENINFO, &fix) < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) < 0) {
            perror("framebuffer info"); goto done;
        }
        if (var.bits_per_pixel != 16 || !var.red.length || !var.green.length || !var.blue.length ||
            var.red.length > 8 || var.green.length > 8 || var.blue.length > 8 ||
            var.red.offset+var.red.length > 16 || var.green.offset+var.green.length > 16 || var.blue.offset+var.blue.length > 16 ||
            var.red.msb_right || var.green.msb_right || var.blue.msb_right || !var.xres || !var.yres ||
            fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR ||
            (uint64_t)(var.yoffset+var.yres)*fix.line_length > fix.smem_len ||
            (uint64_t)(var.xoffset+var.xres)*2 > fix.line_length) {
            fprintf(stderr, "Unsupported framebuffer layout (requires 16-bit truecolor)\n"); goto done;
        }
        dw = var.xres; dh = var.yres;
        if (small && width <= dw && height <= dh) { dw = width; dh = height; }
        else if (!stretch) {
            dw = dh*width/height;
            if (dw > var.xres) { dw = var.xres; dh = dw*height/width; }
        }
        if (!dw || !dh) goto done;
        left = (var.xres-dw)/2; top = (var.yres-dh)/2;
        screen_len = fix.smem_len;
        screen = mmap(NULL, screen_len, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
        if (screen == MAP_FAILED) { perror("mmap framebuffer"); goto done; }
        row = malloc(dw*2);
        if (!row) { perror("malloc"); goto done; }
    }
    struct v4l2_requestbuffers req = {0};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP; req.count = 3;
    if (xioctl(video, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); goto done; }
    if (req.count < 2 || req.count > 4) { fprintf(stderr, "Unexpected capture buffer count\n"); goto done; }
    for (unsigned i=0; i<req.count; ++i) {
        struct v4l2_buffer b = {0};
        b.type = req.type; b.memory = req.memory; b.index = i;
        if (xioctl(video, VIDIOC_QUERYBUF, &b) < 0) { perror("VIDIOC_QUERYBUF"); goto done; }
        void *ptr = mmap(NULL, b.length, PROT_READ|PROT_WRITE, MAP_SHARED, video, b.m.offset);
        if (ptr == MAP_FAILED) { perror("mmap video"); goto done; }
        bufs[i].ptr = ptr; bufs[i].len = b.length; nbuf++;
        if (xioctl(video, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); goto done; }
    }
    if (!no_display) {
        tty = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (tty >= 0 && ioctl(tty, KDGETMODE, &tty_mode) == 0) ioctl(tty, KDSETMODE, KD_GRAPHICS);
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original) == 0) {
            struct termios term = original;
            term.c_lflag &= ~(ICANON|ECHO); term.c_cc[VMIN] = 0; term.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &term) == 0) raw = 1;
        }
        for (unsigned y=0; y<var.yres; ++y)
            memset(screen+(y+var.yoffset)*fix.line_length+var.xoffset*2, 0, var.xres*2);
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(video, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); goto done; }
    streaming = 1;
    struct timespec began, now;
    clock_gettime(CLOCK_MONOTONIC, &began);
    fprintf(stderr, "%s: %ux%u YUYV -> %ux%u%s; q/Esc/Ctrl+C to exit\n", device, width, height, dw, dh, no_display ? " (capture only)" : "");
    unsigned timeouts = 0;
    while (!stop && (!frames || count < frames)) {
        struct pollfd p[2] = {{video, POLLIN, 0}, {raw ? STDIN_FILENO : -1, POLLIN, 0}};
        int ready = poll(p, 2, 2000);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); goto done; }
        if (!ready) { if (++timeouts >= 5) { fprintf(stderr, "Camera timed out\n"); goto done; } continue; }
        if (p[1].revents & POLLIN) {
            char keys[32]; ssize_t n = read(STDIN_FILENO, keys, sizeof(keys));
            for (ssize_t i=0; i<n; ++i) if (keys[i]=='q' || keys[i]=='Q' || keys[i]==27) stop = 1;
        }
        if (p[0].revents & (POLLERR|POLLHUP|POLLNVAL)) { fprintf(stderr, "Camera disconnected or stream error\n"); goto done; }
        if (!(p[0].revents & POLLIN) || stop) continue;
        struct v4l2_buffer b = {0}; b.type = type; b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(video, VIDIOC_DQBUF, &b) < 0) { if (errno==EAGAIN) continue; perror("VIDIOC_DQBUF"); goto done; }
        if (b.index >= nbuf) { fprintf(stderr, "Invalid buffer index\n"); goto done; }
        if (!(b.flags & V4L2_BUF_FLAG_ERROR) && b.bytesused >= (uint64_t)(height-1)*stride+width*2 && b.bytesused <= bufs[b.index].len) {
            if (!no_display) {
                unsigned previous_y = height;
                const uint8_t *src = bufs[b.index].ptr;
                for (unsigned y=0; y<dh; ++y) {
                    unsigned sy = y*height/dh;
                    if (sy != previous_y) {
                        unsigned previous_x = width;
                        uint16_t color = 0;
                        for (unsigned x=0; x<dw; ++x) {
                            unsigned sx = x*width/dw;
                            if (sx != previous_x) {
                                const uint8_t *pair = src+sy*stride+(sx&~1u)*2;
                                color = pixel(pair[(sx&1u)*2], (int)pair[1]-128, (int)pair[3]-128, &var);
                                previous_x = sx;
                            }
                            row[x] = color;
                        }
                        previous_y = sy;
                    }
                    memcpy(screen+(top+y+var.yoffset)*fix.line_length+(left+var.xoffset)*2, row, dw*2);
                }
            }
            count++; timeouts = 0;
            if (count == 1) {
                clock_gettime(CLOCK_MONOTONIC, &now);
                fprintf(stderr, "First frame: %.3f seconds after STREAMON\n", now.tv_sec-began.tv_sec+(now.tv_nsec-began.tv_nsec)/1e9);
            }
        }
        if (xioctl(video, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); goto done; }
    }
    status = 0;
done:
    if (streaming) { enum v4l2_buf_type t=V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(video, VIDIOC_STREAMOFF, &t); }
    for (unsigned i=0; i<nbuf; ++i) munmap(bufs[i].ptr, bufs[i].len);
    if (raw) tcsetattr(STDIN_FILENO, TCSANOW, &original);
    if (tty_mode >= 0) ioctl(tty, KDSETMODE, tty_mode);
    if (tty >= 0) close(tty);
    if (screen != MAP_FAILED) munmap(screen, screen_len);
    free(row);
    if (fb >= 0) close(fb);
    if (video >= 0) close(video);
    fprintf(stderr, "Frames displayed/captured: %u\n", count);
    return stop ? 0 : status;
}
