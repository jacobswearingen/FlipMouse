


// Standard and system headers for device and event handling
#include <dirent.h>      // Directory operations
#include <errno.h>       // Error codes
#include <fcntl.h>       // File control options
#include <libevdev/libevdev-uinput.h> // libevdev for input device emulation
#include <linux/input-event-codes.h>  // Input event codes
#include <linux/input.h> // Input event structures
#include <poll.h>        // Polling file descriptors
#include <signal.h>      // Signal handling
#include <stdio.h>       // Standard I/O
#include <stdlib.h>      // Standard library
#include <string.h>      // String operations
#include <unistd.h>      // Unix standard functions


// Path to input devices
#define DEV_INPUT "/dev/input"
// Maximum number of supported input devices
#define MAX_DEVICES 2
// Log file location
#define LOG_FILE "/cache/FlipMouse.log"
// Minimum mouse speed
#define MIN_MOUSE_SPEED 1
// Factor to slow down wheel events
#define WHEEL_SLOWDOWN_FACTOR 5


// Structure representing a physical input device
typedef struct {
  int fd;                        // File descriptor for the device
  struct libevdev *evdev;        // libevdev device handle
  struct libevdev_uinput *uidev; // uinput device for event injection
} device_t;

// Structure representing the virtual mouse state
typedef struct {
  int enabled;                   // Mouse mode enabled/disabled
  int speed;                     // Mouse movement speed
  int drag_mode;                 // Drag mode state
  struct libevdev *dev;          // Virtual mouse device
  struct libevdev_uinput *uidev; // Virtual mouse uinput device
} mouse_t;

// Global application state
typedef struct {
  device_t devices[MAX_DEVICES]; // Array of supported devices
  int device_count;              // Number of attached devices
  mouse_t mouse;                 // Virtual mouse state
  FILE *log_fp;                  // Log file pointer
  volatile sig_atomic_t running; // Main loop running flag
} app_state_t;


// Global instance of application state
static app_state_t app = {0};


// Check if the device name is one of the supported keypads
static int is_supported_device(const char *name) {
  return strcmp(name, "mtk-kpd") == 0 || strcmp(name, "matrix-keypad") == 0;
}


// Keymap: maps keypad scancodes to Linux input keycodes
static const struct {
  int scancode, keycode;
} keymap[] = {
  {35, KEY_UP},    // Up arrow
  {9,  KEY_DOWN},  // Down arrow
  {19, KEY_LEFT},  // Left arrow
  {34, KEY_RIGHT}, // Right arrow
  {33, KEY_MENU},  // Menu key
  {2,  KEY_SEND}   // Send key
};


// Convert a keypad scancode to a Linux keycode
static int scan_to_key(int scan) {
  for (size_t i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i)
    if (keymap[i].scancode == scan)
      return keymap[i].keycode;
  return -1;
}

// Convert a Linux keycode to a keypad scancode
static int key_to_scan(int key) {
  for (size_t i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i)
    if (keymap[i].keycode == key)
      return keymap[i].scancode;
  return -1;
}


// Logging macro: writes formatted log messages to the log file if open
#define LOG(fmt, ...)                                                          \
  do {                                                                         \
    if (app.log_fp) {                                                          \
      fprintf(app.log_fp, fmt "\n", ##__VA_ARGS__);                            \
      fflush(app.log_fp);                                                      \
    }                                                                          \
  } while (0)


// Open the log file for appending and log startup
static void log_init(void) {
  app.log_fp = fopen(LOG_FILE, "a");
  LOG("\n----- FlipMouse Log initialized -----");
}

// Close the log file if open
static void log_close(void) {
  if (app.log_fp)
    fclose(app.log_fp);
  app.log_fp = NULL;
}


// Initialize the virtual mouse device using libevdev-uinput
static int mouse_init(void) {
  app.mouse.dev = libevdev_new();
  if (!app.mouse.dev) {
    LOG("ERROR: Failed to create virtual mouse device");
    return -1;
  }
  // Set device name and enable supported event codes
  libevdev_set_name(app.mouse.dev, "FlipMouse Virtual Mouse");
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_X, NULL);      // X movement
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_Y, NULL);      // Y movement
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_WHEEL, NULL);  // Vertical wheel
  libevdev_enable_event_code(app.mouse.dev, EV_REL, REL_HWHEEL, NULL); // Horizontal wheel
  libevdev_enable_event_code(app.mouse.dev, EV_KEY, BTN_LEFT, NULL);   // Left button
  libevdev_enable_event_code(app.mouse.dev, EV_KEY, BTN_RIGHT, NULL);  // Right button
  // Create the uinput device for event injection
  if (libevdev_uinput_create_from_device(
          app.mouse.dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &app.mouse.uidev) < 0) {
    LOG("ERROR: Failed to create virtual mouse uinput device");
    libevdev_free(app.mouse.dev);
    app.mouse.dev = NULL;
    return -1;
  }
  // Set initial mouse state
  app.mouse.enabled = 0;
  app.mouse.speed = 4;
  app.mouse.drag_mode = 0;
  LOG("Virtual mouse initialized");
  return 0;
}

// Release resources for the virtual mouse
static void mouse_cleanup(void) {
  if (app.mouse.uidev)
    libevdev_uinput_destroy(app.mouse.uidev);
  if (app.mouse.dev)
    libevdev_free(app.mouse.dev);
  app.mouse.uidev = NULL;
  app.mouse.dev = NULL;
  LOG("Virtual mouse resources released");
}

// Toggle mouse mode (enable/disable)
static int mouse_toggle(void) {
  app.mouse.enabled = !app.mouse.enabled;
  LOG("Mouse mode %s", app.mouse.enabled ? "enabled" : "disabled");
  return -2; // Special return value for event handling
}


// Scan /dev/input for supported devices and initialize them
static int devices_find_and_init(void) {
  DIR *dir = opendir(DEV_INPUT);
  if (!dir) {
    LOG("ERROR: open %s", DEV_INPUT);
    return -1;
  }
  app.device_count = 0;
  struct dirent *file;
  // Iterate over directory entries
  while ((file = readdir(dir)) && app.device_count < MAX_DEVICES) {
    if (file->d_type != DT_CHR)
      continue; // Only consider character devices
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", DEV_INPUT, file->d_name);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      LOG("ERROR: open %s", path);
      continue;
    }
    struct libevdev *evdev = NULL;
    // Create libevdev device from file descriptor
    if (libevdev_new_from_fd(fd, &evdev) < 0) {
      close(fd);
      continue;
    }
    const char *devname = libevdev_get_name(evdev);
    // Check if device is supported
    if (!is_supported_device(devname)) {
      libevdev_free(evdev);
      close(fd);
      continue;
    }
    device_t *dev = &app.devices[app.device_count];
    dev->fd = fd;
    dev->evdev = evdev;
    dev->uidev = NULL;
    // Grab exclusive access to the device
    if (ioctl(dev->fd, EVIOCGRAB, 1) < 0)
      LOG("WARNING: Failed to grab device");
    // Create uinput device for event injection
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

// Release all attached input devices
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


// Handle a single input event and translate it to mouse actions if needed
static int mouse_handle_event(struct input_event *ev) {
  static unsigned int slowdown = 0; // Used to slow down wheel events
  int keycode = ev->code;

  // If event is a scancode, map it to a keycode
  if (ev->type == EV_MSC && keycode == MSC_SCAN) {
    int mapped = scan_to_key(ev->value);
    if (mapped != -1)
      keycode = mapped;
  } else if (ev->type == EV_KEY && key_to_scan(keycode) != -1) {
    // Ignore key events that are just keypad scancodes
    return 0;
  }

  // Map keycodes to mouse actions or special functions
  switch (keycode) {
  case KEY_VOLUMEUP:
    // Increase mouse speed on key press
    if (ev->value == 1) {
      app.mouse.speed++;
      LOG("Mouse speed: %d", app.mouse.speed);
    }
    return 0;
  case KEY_VOLUMEDOWN:
    // Decrease mouse speed on key press
    if (ev->value == 1) {
      app.mouse.speed--;
      if (app.mouse.speed < MIN_MOUSE_SPEED)
        app.mouse.speed = MIN_MOUSE_SPEED;
      LOG("Mouse speed: %d", app.mouse.speed);
    }
    return 0;
  case KEY_ENTER:
    // Map Enter key to left mouse button click
    ev->type = EV_KEY;
    ev->code = BTN_LEFT;
    return -2;
  case KEY_B:
    // Toggle drag mode on B key press
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
    // Move mouse up
    ev->type = EV_REL;
    ev->code = REL_Y;
    ev->value = -app.mouse.speed;
    return -2;
  case KEY_DOWN:
    // Move mouse down
    ev->type = EV_REL;
    ev->code = REL_Y;
    ev->value = app.mouse.speed;
    return -2;
  case KEY_LEFT:
    // Move mouse left
    ev->type = EV_REL;
    ev->code = REL_X;
    ev->value = -app.mouse.speed;
    return -2;
  case KEY_RIGHT:
    // Move mouse right
    ev->type = EV_REL;
    ev->code = REL_X;
    ev->value = app.mouse.speed;
    return -2;
  case KEY_MENU:
    // Scroll mouse wheel up, but slow down repeated events
    if (slowdown++ % WHEEL_SLOWDOWN_FACTOR)
      return 0;
    ev->type = EV_REL;
    ev->code = REL_WHEEL;
    ev->value = 1;
    return -2;
  case KEY_SEND:
    // Scroll mouse wheel down, but slow down repeated events
    if (slowdown++ % WHEEL_SLOWDOWN_FACTOR)
      return 0;
    ev->type = EV_REL;
    ev->code = REL_WHEEL;
    ev->value = -1;
    return -2;
  default:
    // Unhandled keycode: pass through
    return 1;
  }
  return 1;
}


// Handle an input event from a device, possibly toggling mouse mode or passing to mouse handler
static int handle_input_event(device_t *dev, struct input_event *ev) {
  // Special keys to toggle mouse mode
  if (ev->type == EV_KEY && (ev->code == KEY_HELP || ev->code == KEY_F12)) {
    if (ev->value == 1)
      return mouse_toggle();
    return -2;
  }
  // If mouse mode is not enabled, pass event through
  if (!app.mouse.enabled)
    return 1;
  // Otherwise, handle as mouse event
  return mouse_handle_event(ev);
}


// Signal handler: log and set running flag to 0 to exit main loop
static void signal_handler(int sig) {
  LOG("Signal %d, shutting down", sig);
  app.running = 0;
}

// Register signal handlers for clean shutdown
static void setup_signals(void) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGHUP, signal_handler);
}


// Main event loop: poll devices for input and process events
static int run_event_loop(void) {
  struct input_event event;
  struct pollfd pfds[MAX_DEVICES];
  LOG("Entering main event loop");
  app.running = 1;
  // Set up pollfd structures for each device
  for (int i = 0; i < app.device_count; i++) {
    pfds[i].fd = app.devices[i].fd;
    pfds[i].events = POLLIN;
  }
  // Main loop: poll for events while running
  while (app.running) {
    int ret = poll(pfds, app.device_count, 500); // 500ms timeout
    if (ret < 0) {
      if (errno == EINTR)
        continue; // Interrupted by signal
      LOG("ERROR: poll() failed");
      break;
    }
    if (ret == 0)
      continue; // Timeout, no events
    // Check each device for input
    for (int i = 0; i < app.device_count; i++) {
      if (!(pfds[i].revents & POLLIN))
        continue;
      ssize_t r = read(app.devices[i].fd, &event, sizeof(event));
      if (r != sizeof(event)) {
        LOG("ERROR: read event");
        continue;
      }
      // Handle the input event
      int res = handle_input_event(&app.devices[i], &event);
      if (res > 0) {
        // Pass event through to device's uinput
        libevdev_uinput_write_event(app.devices[i].uidev, event.type,
                                    event.code, event.value);
        libevdev_uinput_write_event(app.devices[i].uidev, EV_SYN, SYN_REPORT,
                                    0);
      } else if (res < 0) {
        // Inject event into virtual mouse
        libevdev_uinput_write_event(app.mouse.uidev, event.type, event.code,
                                    event.value);
        libevdev_uinput_write_event(app.mouse.uidev, EV_SYN, SYN_REPORT, 0);
      }
    }
  }
  return 0;
}


// Program entry point
int main(void) {
  log_init(); // Open log file
  LOG("FlipMouse starting up");
  setup_signals(); // Register signal handlers

  // Initialize input devices
  if (devices_find_and_init() != 0) {
    LOG("ERROR: No supported input devices");
    log_close();
    return 1;
  }

  // Initialize virtual mouse
  if (mouse_init() != 0) {
    LOG("ERROR: Virtual mouse init failed");
    devices_cleanup();
    log_close();
    return 1;
  }

  // Run the main event loop
  int result = run_event_loop();

  // Cleanup resources
  mouse_cleanup();
  devices_cleanup();
  LOG("FlipMouse shutting down");
  log_close();
  return result;
}