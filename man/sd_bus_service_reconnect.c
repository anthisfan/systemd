/* SPDX-License-Identifier: MIT-0 */

/* Implements a D-Bus service that automatically reconnects when the system bus is restarted.
 *
 * Compile with 'cc sd_bus_service_reconnect.c $(pkg-config --libs --cflags libsystemd)'
 *
 * To allow the program to take ownership of the name 'org.freedesktop.ReconnectExample',
 * add the following as /etc/dbus-1/system.d/org.freedesktop.ReconnectExample.conf:

<?xml version="1.0"?> <!--*-nxml-*-->
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
        "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
        <policy user="root">
                <allow own="org.freedesktop.ReconnectExample"/>
                <allow send_destination="org.freedesktop.ReconnectExample"/>
                <allow receive_sender="org.freedesktop.ReconnectExample"/>
        </policy>

        <policy context="default">
                <allow send_destination="org.freedesktop.ReconnectExample"/>
                <allow receive_sender="org.freedesktop.ReconnectExample"/>
        </policy>
</busconfig>

 *
 * To get the property via busctl:
 *
 * $ busctl --user get-property org.freedesktop.ReconnectExample \
 *                              /org/freedesktop/ReconnectExample \
 *                              org.freedesktop.ReconnectExample \
 *                              Example
 *   s "example"
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-bus.h>

#define _cleanup_(f) __attribute__((cleanup(f)))

#define check(x) ({                             \
  int _r = (x);                                 \
  errno = _r < 0 ? -_r : 0;                     \
  printf(#x ": %m\n");                          \
  if (_r < 0)                                   \
    return EXIT_FAILURE;                        \
  })

typedef struct object {
  const char *example;
  sd_bus **bus;
  sd_event **event;
} object;

static int property_get(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

  object *o = userdata;

  if (strcmp(property, "Example") == 0)
    return sd_bus_message_append(reply, "s", o->example);

  return sd_bus_error_setf(error,
                           SD_BUS_ERROR_UNKNOWN_PROPERTY,
                           "Unknown property '%s'",
                           property);
}

/* https://www.freedesktop.org/software/systemd/man/sd_bus_add_object.html */
static const sd_bus_vtable vtable[] = {
  SD_BUS_VTABLE_START(0),
  SD_BUS_PROPERTY(
    "Example", "s",
    property_get,
    0,
    SD_BUS_VTABLE_PROPERTY_CONST),
  SD_BUS_VTABLE_END
};

static int setup(object *o);

static int on_disconnect(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
  check(setup((object *)userdata));
  return 0;
}

static int setup(object *o) {
  /* If we are reconnecting, then the bus object needs to be closed, detached from
   * the event loop and recreated.
   * https://www.freedesktop.org/software/systemd/man/sd_bus_detach_event.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_close_unref.html
   */
  if (*o->bus) {
    check(sd_bus_detach_event(*o->bus));
    *o->bus = sd_bus_close_unref(*o->bus);
  }

  /* Set up a new bus object for the system bus, configure it to wait for D-Bus to be available
   * instead of failing if it is not, and start it. All the following operations are asynchronous
   * and will not block waiting for D-Bus to be available.
   * https://www.freedesktop.org/software/systemd/man/sd_bus_new.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_set_address.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_set_bus_client.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_negotiate_creds.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_set_watch_bind.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_set_connected_signal.html
   * https://www.freedesktop.org/software/systemd/man/sd_bus_start.html
   */
  check(sd_bus_new(o->bus));
  check(sd_bus_set_address(*o->bus, "unix:path=/run/dbus/system_bus_socket"));
  check(sd_bus_set_bus_client(*o->bus, 1));
  check(sd_bus_negotiate_creds(*o->bus, 1, SD_BUS_CREDS_UID|SD_BUS_CREDS_EUID|SD_BUS_CREDS_EFFECTIVE_CAPS));
  check(sd_bus_set_watch_bind(*o->bus, 1));
  check(sd_bus_set_connected_signal(*o->bus, 1));
  check(sd_bus_start(*o->bus));

  /* Publish an interface on the bus, specifying our well-known object access
   * path and public interface name.
   * https://www.freedesktop.org/software/systemd/man/sd_bus_add_object.html
   * https://dbus.freedesktop.org/doc/dbus-tutorial.html
   */
  check(sd_bus_add_object_vtable(*o->bus,
                                 NULL,
                                 "/org/freedesktop/ReconnectExample",
                                 "org.freedesktop.ReconnectExample",
                                 vtable,
                                 o));
  /* By default the service is only assigned an ephemeral name. Also add a well-known
   * one, so that clients know whom to call. This needs to be asynchronous, as
   * D-Bus might not be yet available.
   * https://www.freedesktop.org/software/systemd/man/sd_bus_request_name.html
   */
  check(sd_bus_request_name_async(*o->bus,
                                  NULL,
                                  "org.freedesktop.ReconnectExample",
                                  0,
                                  NULL,
                                  NULL));
  /* When D-Bus is disconnected this callback will be invoked, which will
   * set up the connection again. This needs to be asynchronous, as D-Bus might not
   * yet be available.
   * https://www.freedesktop.org/software/systemd/man/sd_bus_match_signal_async.html
   */
  check(sd_bus_match_signal_async(*o->bus,
                                  NULL,
                                  "org.freedesktop.DBus.Local",
                                  NULL,
                                  "org.freedesktop.DBus.Local",
                                  "Disconnected",
                                  on_disconnect,
                                  NULL,
                                  o));
  /* Attach the bus object to the event loop so that calls and signals are processed.
   * https://www.freedesktop.org/software/systemd/man/sd_bus_attach_event.html
   */
  check(sd_bus_attach_event(*o->bus, *o->event, 0));

  return 0;
}

int main(int argc, char **argv) {
  /* The bus should be relinquished before the program terminates. The cleanup
   * attribute allows us to do it nicely and cleanly whenever we exit the
   * block.
   */
  _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
  _cleanup_(sd_event_unrefp) sd_event *event = NULL;
  object o = {
    .example = "example",
    .bus = &bus,
    .event = &event,
  };

  /* Create an event loop data structure, with default parameters.
   * https://www.freedesktop.org/software/systemd/man/sd_event_default.html
   */
  check(sd_event_default(&event));

  /* By default the event loop will terminate when all sources have disappeared, so
   * we have to keep it 'occupied'. Register signal handling to do so.
   * https://www.freedesktop.org/software/systemd/man/sd_event_add_signal.html
   */
  check(sd_event_add_signal(event, NULL, SIGINT|SD_EVENT_SIGNAL_PROCMASK, NULL, NULL));
  check(sd_event_add_signal(event, NULL, SIGTERM|SD_EVENT_SIGNAL_PROCMASK, NULL, NULL));

  check(setup(&o));

  /* Enter the main loop, it will exit only on sigint/sigterm.
   * https://www.freedesktop.org/software/systemd/man/sd_event_loop.html
   */
  check(sd_event_loop(event));

  /* https://www.freedesktop.org/software/systemd/man/sd_bus_release_name.html */
  check(sd_bus_release_name(bus, "org.freedesktop.ReconnectExample"));

  return 0;
}
