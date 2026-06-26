#include "clipboard.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pixbuf.h"
#include "util.h"

#define gtk_clipboard_t GtkClipboard
#define gdk_pixbuf_t GdkPixbuf

static bool send_bytes_to_wl_copy(const void *buffer, size_t size, const char *mime_type) {
  pid_t clipboard_process = 0;
  int pipefd[2];
  int status;
  ssize_t written;

  if (pipe(pipefd) < 0) {
    g_warning("unable to pipe for copy process to work");
    return false;
  }
  clipboard_process = fork();
  if (clipboard_process == -1) {
    g_warning("unable to fork process for copy");
    return false;
  }
  if (clipboard_process == 0) {
    close(pipefd[1]);
    dup2(pipefd[0], STDIN_FILENO);
    close(pipefd[0]);
    execlp("wl-copy", "wl-copy", "-t", "image/png", NULL);
    g_warning("Failed to copy content to wl-copy: %s", g_strerror(errno));
    exit(1);
  }
  close(pipefd[0]);

  // actual write
  written = write(pipefd[1], buffer, size);
  if (written == -1) {
    g_warning("unable to write to pipe fd for copy");
    return false;
  }

  close(pipefd[1]);
  waitpid(clipboard_process, &status, 0);

  if (WIFEXITED(status)) {
    return WEXITSTATUS(status) == 0;  // Make sure the child exited properly
  }

  return false;
}

static gboolean send_pixbuf_to_wl_copy(gdk_pixbuf_t *pixbuf) {
  gsize size;
  gchar *buffer = NULL;
  GError *error = NULL;

  gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &size, "png", &error, NULL);
  if (error != NULL) {
    g_critical("unable to save pixbuf to buffer for copy: %s", error->message);
    g_error_free(error);
    return false;
  }

  bool success = send_bytes_to_wl_copy(buffer, size, "image/png");

  g_free(buffer);

  return success;
}

static void send_pixbuf_to_gdk_clipboard(gdk_pixbuf_t *pixbuf) {
  gtk_clipboard_t *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_image(clipboard, pixbuf);
  gtk_clipboard_store(clipboard);  // Does not work for Wayland gdk backend
}

static void send_text_to_gdk_clipboard(char *utf8Buf, size_t length) {
  gtk_clipboard_t *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(clipboard, utf8Buf, length);
  gtk_clipboard_store(clipboard);  // Does not work for Wayland gdk backend
}

bool clipboard_copy_drawing_area_to_selection(struct swappy_state *state) {
  gdk_pixbuf_t *pixbuf = pixbuf_get_from_state(state);

  // Try `wl-copy` first and fall back to gtk function. See README.md.
  if (!send_pixbuf_to_wl_copy(pixbuf)) {
    send_pixbuf_to_gdk_clipboard(pixbuf);
  }

  g_object_unref(pixbuf);

  if (state->config->early_exit)
    gtk_main_quit();

  return true;
}

bool clipboard_copy_text_to_gdk_clipboard(char *text) {
  size_t len = strlen(text);
  bool success = send_bytes_to_wl_copy(text, len, "text/plain");

  // fall back
  if (!success)
    send_text_to_gdk_clipboard(text, len);

  return true;
}
