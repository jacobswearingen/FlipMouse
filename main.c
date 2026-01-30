

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev-uinput.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEV_INPUT "/dev/input"
#define MAX_DEVICES 2
#define LOG_FILE "/cache/FlipMouse.log"
#define MIN_MOUSE_SPEED 1
#define WHEEL_SLOWDOWN_FACTOR 5

typedef struct {
  int fd;
  struct libevdev *evdev;
  struct libevdev_uinput *uidev;
} device_t;

typedef struct {
  int enabled, speed, drag_mode;
  struct libevdev *dev;
  struct libevdev_uinput *uidev;
} mouse_t;

typedef struct {
  device_t devices[MAX_DEVICES];
  int device_count;
  mouse_t mouse;
  FILE *log_fp;
  volatile sig_atomic_t running;
} app_state_t;

static app_state_t app = {0};

// Only support these two device names
static int is_supported_device(const char *name) {
  return strcmp(name, "mtk-kpd") == 0 || strcmp(name, "matrix-keypad") == 0;
}

// Keymap for supported keypads
static const struct {
  int scancode, keycode;
} keymap[] = {{35, KEY_UP},    {9, KEY_DOWN},  {19, KEY_LEFT},
              {34, KEY_RIGHT}, {33, KEY_MENU}, {2, KEY_SEND}};

static int scan_to_key(int scan) {
  for (size_t i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i)
    if (keymap[i].scancode == scan)
      return keymap[i].keycode;
  return -1;
}
static int key_to_scan(int key) {
  for (size_t i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i)
    if (keymap[i].keycode == key)
      return keymap[i].scancode;
  return -1;
}

#define LOG(fmt, ...)                                                          \
  do {                                                                         \
    if (app.log_fp) {                                                          \
      fprintf(app.log_fp, fmt "\n", ##__VA_ARGS__);                            \
      fflush(app.log_fp);                                                      \
    }                                                                          \
  } while (0)

static void log_init(void) {
  app.log_fp = fopen(LOG_FILE, "a");
  LOG("\n----- FlipMouse Log initialized -----");
}
static void log_close(void) {
  if (app.log_fp)
    fclose(app.log_fp);
  app.log_fp = NULL;
}

static int mouse_init(void) {
  app.mouse.dev = libevdev_new();
  if (!app.mouse.dev) {
    LOG("ERROR: Failed to create virtual mouse device");
    return -1;
  }
  libevdev_set_name(app.mouse.dev, "FlipMouse Virtual Mouse");
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_X, NULL);
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_Y, NULL);
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_WHEEL, NULL);
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_HWHEEL, NULL);
  libevdev_enable_event_code(app.mouse.dev, EV_KEY, BTN_LEFT, NULL);
  libevdev_enable_event_code(app.mouse.dev, EV_KEY, BTN_RIGHT, NULL);
  if (libevdev_uinput_create_from_device(
          app.mouse.dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &app.mouse.uidev) < 0) {
    LOG("ERROR: Failed to create virtual mouse uinput device");
    libevdev_free(app.mouse.dev);
    app.mouse.dev = NULL;
    return -1;
  }
  app.mouse.enabled = 0;
  app.mouse.speed = 4;
  app.mouse.drag_mode = 0;
  LOG("Virtual mouse initialized");
  return 0;
}
static void mouse_cleanup(void) {
  if (app.mouse.uidev)
    libevdev_uinput_destroy(app.mouse.uidev);
  if (app.mouse.dev)
    libevdev_free(app.mouse.dev);
  app.mouse.uidev = NULL;
  app.mouse.dev = NULL;
  LOG("Virtual mouse resources released");
}
static int mouse_toggle(void) {
  app.mouse.enabled = !app.mouse.enabled;
  LOG("Mouse mode %s", app.mouse.enabled ? "enabled" : "disabled");
  return -2;
}

static int devices_find_and_init(void) {
  DIR *dir = opendir(DEV_INPUT);
  if (!dir) {
    LOG("ERROR: open %s", DEV_INPUT);
    return -1;
  }
  app.device_count = 0;
  struct dirent *file;
  while ((file = readdir(dir)) && app.device_count < MAX_DEVICES) {
    if (file->d_type != DT_CHR)
      continue;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", DEV_INPUT, file->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      LOG("ERROR: open %s", path);
      continue;
    }
    struct libevdev *evdev = NULL;
    if (libevdev_new_from_fd(fd, &evdev) < 0) {
      close(fd);
      continue;
    }
    const char *devname = libevdev_get_name(evdev);
    if (!is_supported_device(devname)) {
      libevdev_free(evdev);
      close(fd);
      continue;
    }
    device_t *dev = &app.devices[app.device_count];
    dev->fd = fd;
    dev->evdev = evdev;
    dev->uidev = NULL;
    if (ioctl(dev->fd, EVIOCGRAB, 1) < 0)
      LOG("WARNING: Failed to grab device");
    if (libevdev_uinput_create_from_device(
            dev->evdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &dev->uidev) < 0) {
      libevdev_free(evdev);
      close(fd);
      continue;
    }
    LOG("Attached device: %s", devname);
    app.device_count++;
  }
  closedir(dir);
  return app.device_count > 0 ? 0 : -1;
}
static void devices_cleanup(void) {
  for (int i = 0; i < app.device_count; i++) {
    if (app.devices[i].uidev)
      libevdev_uinput_destroy(app.devices[i].uidev);
    if (app.devices[i].evdev)
      libevdev_free(app.devices[i].evdev);
    if (app.devices[i].fd >= 0)
      close(app.devices[i].fd);
  }
  app.device_count = 0;
  LOG("All input devices released");
}

static int mouse_handle_event(struct input_event *ev) {
  static unsigned int slowdown = 0;
  int keycode = ev->code;
  if (ev->type == EV_MSC && keycode == MSC_SCAN) {
    int mapped = scan_to_key(ev->value);
    if (mapped != -1)
      keycode = mapped;
  } else if (ev->type == EV_KEY && key_to_scan(keycode) != -1) {
    return 0;
  }
  switch (keycode) {
  case KEY_VOLUMEUP:
    if (ev->value == 1) {
      app.mouse.speed++;
      LOG("Mouse speed: %d", app.mouse.speed);
    }
    return 0;
  case KEY_VOLUMEDOWN:
    if (ev->value == 1) {
      app.mouse.speed--;
      if (app.mouse.speed < MIN_MOUSE_SPEED)
        app.mouse.speed = MIN_MOUSE_SPEED;
      LOG("Mouse speed: %d", app.mouse.speed);
    }
    return 0;
  case KEY_ENTER:
    ev->type = EV_KEY;
    ev->code = BTN_LEFT;
    return -2;
  case KEY_B:
    if (ev->value == 1) {
      app.mouse.drag_mode = !app.mouse.drag_mode;
      ev->type = EV_KEY;
      ev->code = BTN_LEFT;
      ev->value = app.mouse.drag_mode ? 1 : 0;
      LOG("Drag mode %s", app.mouse.drag_mode ? "on" : "off");
      return -2;
    }
    break;
  case KEY_UP:
    ev->type = EV_REL;
    ev->code = REL_Y;
    ev->value = -app.mouse.speed;
    return -2;
  case KEY_DOWN:
    ev->type = EV_REL;
    ev->code = REL_Y;
    ev->value = app.mouse.speed;
    return -2;
  case KEY_LEFT:
    ev->type = EV_REL;
    ev->code = REL_X;
    ev->value = -app.mouse.speed;
    return -2;
  case KEY_RIGHT:
    ev->type = EV_REL;
    ev->code = REL_X;
    ev->value = app.mouse.speed;
    return -2;
  case KEY_MENU:
    if (slowdown++ % WHEEL_SLOWDOWN_FACTOR)
      return 0;
    ev->type = EV_REL;
    ev->code = REL_WHEEL;
    ev->value = 1;
    return -2;
  case KEY_SEND:
    if (slowdown++ % WHEEL_SLOWDOWN_FACTOR)
      return 0;
    ev->type = EV_REL;
    ev->code = REL_WHEEL;
    ev->value = -1;
    return -2;
  default:
    return 1;
  }
  return 1;
}

static int handle_input_event(device_t *dev, struct input_event *ev) {
  if (ev->type == EV_KEY && (ev->code == KEY_HELP || ev->code == KEY_F12)) {
    if (ev->value == 1)
      return mouse_toggle();
    return -2;
  }
  if (!app.mouse.enabled)
    return 1;
  return mouse_handle_event(ev);
}

static void signal_handler(int sig) {
  LOG("Signal %d, shutting down", sig);
  app.running = 0;
}
static void setup_signals(void) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
}

static int run_event_loop(void) {
  struct input_event event;
  struct pollfd pfds[MAX_DEVICES];
  LOG("Entering main event loop");
  app.running = 1;
  for (int i = 0; i < app.device_count; i++) {
    pfds[i].fd = app.devices[i].fd;
    pfds[i].events = POLLIN;
  }
  while (app.running) {
    int ret = poll(pfds, app.device_count, 500);
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      LOG("ERROR: poll() failed");
      break;
    }
    if (ret == 0)
      continue;
    for (int i = 0; i < app.device_count; i++) {
      if (!(pfds[i].revents & POLLIN))
        continue;
      ssize_t r = read(app.devices[i].fd, &event, sizeof(event));
      if (r != sizeof(event)) {
        LOG("ERROR: read event");
        continue;
      }
      int res = handle_input_event(&app.devices[i], &event);
      if (res > 0) {
        libevdev_uinput_write_event(app.devices[i].uidev, event.type,
                                    event.code, event.value);
        libevdev_uinput_write_event(app.devices[i].uidev, EV_SYN, SYN_REPORT,
                                    0);
      } else if (res < 0) {
        libevdev_uinput_write_event(app.mouse.uidev, event.type, event.code,
                                    event.value);
        libevdev_uinput_write_event(app.mouse.uidev, EV_SYN, SYN_REPORT, 0);
      }
    }
  }
  return 0;
}

int main(void) {
  log_init();
  LOG("FlipMouse starting up");
  setup_signals();
  if (devices_find_and_init() != 0) {
    LOG("ERROR: No supported input devices");
    log_close();
    return 1;
  }
  if (mouse_init() != 0) {
    LOG("ERROR: Virtual mouse init failed");
    devices_cleanup();
    log_close();
    return 1;
  }
  int result = run_event_loop();
  mouse_cleanup();
  devices_cleanup();
  LOG("FlipMouse shutting down");
  log_close();
  return result;
}