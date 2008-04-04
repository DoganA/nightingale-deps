/* GStreamer
 * Copyright (C) <2007> Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-multiupdsink
 * @see_also: udpsink, multifdsink
 *
 * <refsect2>
 * <para>
 * multiudpsink is a network sink that sends UDP packets to multiple
 * clients.
 * It can be combined with rtp payload encoders to implement RTP streaming.
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstudp-marshal.h"
#include "gstmultiudpsink.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (multiudpsink_debug);
#define GST_CAT_DEFAULT (multiudpsink_debug)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

/* elementfactory information */
static const GstElementDetails gst_multiudpsink_details =
GST_ELEMENT_DETAILS ("UDP packet sender",
    "Sink/Network",
    "Send data over the network via UDP",
    "Wim Taymans <wim.taymans@gmail.com>");

/* MultiUDPSink signals and args */
enum
{
  /* methods */
  SIGNAL_ADD,
  SIGNAL_REMOVE,
  SIGNAL_CLEAR,
  SIGNAL_GET_STATS,

  /* signals */
  SIGNAL_CLIENT_ADDED,
  SIGNAL_CLIENT_REMOVED,

  /* FILL ME */
  LAST_SIGNAL
};

#define DEFAULT_SOCKFD             -1
#define DEFAULT_CLOSEFD            TRUE
#define DEFAULT_SOCK               -1
#define DEFAULT_CLIENTS            NULL
#define DEFAULT_AUTO_MULTICAST     TRUE

enum
{
  PROP_0,
  PROP_BYTES_TO_SERVE,
  PROP_BYTES_SERVED,
  PROP_SOCKFD,
  PROP_CLOSEFD,
  PROP_SOCK,
  PROP_CLIENTS,
  PROP_AUTO_MULTICAST
      /* FILL ME */
};

#define CLOSE_IF_REQUESTED(udpctx)                                        \
  if ((!udpctx->externalfd) || (udpctx->externalfd && udpctx->closefd))   \
    CLOSE_SOCKET(udpctx->sock);                                           \
  udpctx->sock = -1;

static void gst_multiudpsink_base_init (gpointer g_class);
static void gst_multiudpsink_class_init (GstMultiUDPSinkClass * klass);
static void gst_multiudpsink_init (GstMultiUDPSink * udpsink);
static void gst_multiudpsink_finalize (GObject * object);

static GstFlowReturn gst_multiudpsink_render (GstBaseSink * sink,
    GstBuffer * buffer);
static GstStateChangeReturn gst_multiudpsink_change_state (GstElement *
    element, GstStateChange transition);

static void gst_multiudpsink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_multiudpsink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void gst_multiudpsink_add_internal (GstMultiUDPSink * sink,
    const gchar * host, gint port, gboolean lock);
static void gst_multiudpsink_clear_internal (GstMultiUDPSink * sink,
    gboolean lock);

static void free_client (GstUDPClient * client);

static GstElementClass *parent_class = NULL;

static guint gst_multiudpsink_signals[LAST_SIGNAL] = { 0 };

GType
gst_multiudpsink_get_type (void)
{
  static GType multiudpsink_type = 0;

  if (!multiudpsink_type) {
    static const GTypeInfo multiudpsink_info = {
      sizeof (GstMultiUDPSinkClass),
      gst_multiudpsink_base_init,
      NULL,
      (GClassInitFunc) gst_multiudpsink_class_init,
      NULL,
      NULL,
      sizeof (GstMultiUDPSink),
      0,
      (GInstanceInitFunc) gst_multiudpsink_init,
      NULL
    };

    multiudpsink_type =
        g_type_register_static (GST_TYPE_BASE_SINK, "GstMultiUDPSink",
        &multiudpsink_info, 0);
  }
  return multiudpsink_type;
}

static void
gst_multiudpsink_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_template));

  gst_element_class_set_details (element_class, &gst_multiudpsink_details);
}

static void
gst_multiudpsink_class_init (GstMultiUDPSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = gst_multiudpsink_set_property;
  gobject_class->get_property = gst_multiudpsink_get_property;
  gobject_class->finalize = gst_multiudpsink_finalize;

  /**
   * GstMultiUDPSink::add:
   * @gstmultiudpsink: the sink on which the signal is emitted
   * @host: the hostname/IP address of the client to add
   * @port: the port of the client to add
   *
   * Add a client with destination @host and @port to the list of
   * clients.
   */
  gst_multiudpsink_signals[SIGNAL_ADD] =
      g_signal_new ("add", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstMultiUDPSinkClass, add),
      NULL, NULL, gst_udp_marshal_VOID__STRING_INT, G_TYPE_NONE, 2,
      G_TYPE_STRING, G_TYPE_INT);
  /**
   * GstMultiUDPSink::remove:
   * @gstmultiudpsink: the sink on which the signal is emitted
   * @host: the hostname/IP address of the client to remove
   * @port: the port of the client to remove
   *
   * Remove the client with destination @host and @port from the list of
   * clients.
   */
  gst_multiudpsink_signals[SIGNAL_REMOVE] =
      g_signal_new ("remove", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstMultiUDPSinkClass, remove),
      NULL, NULL, gst_udp_marshal_VOID__STRING_INT, G_TYPE_NONE, 2,
      G_TYPE_STRING, G_TYPE_INT);
  /**
   * GstMultiUDPSink::clear:
   * @gstmultiudpsink: the sink on which the signal is emitted
   *
   * Clear the list of clients.
   */
  gst_multiudpsink_signals[SIGNAL_CLEAR] =
      g_signal_new ("clear", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstMultiUDPSinkClass, clear),
      NULL, NULL, g_cclosure_marshal_VOID__VOID, G_TYPE_NONE, 0);
  /**
   * GstMultiUDPSink::get-stats:
   * @gstmultiudpsink: the sink on which the signal is emitted
   * @host: the hostname/IP address of the client to get stats on
   * @port: the port of the client to get stats on
   *
   * Get the statistics of the client with destination @host and @port.
   *
   * Returns: a GValueArray of uint64: bytes_sent, packets_sent,
   *           connect_time (in epoch seconds), disconnect_time (in epoch seconds)
   */
  gst_multiudpsink_signals[SIGNAL_GET_STATS] =
      g_signal_new ("get-stats", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
      G_STRUCT_OFFSET (GstMultiUDPSinkClass, get_stats),
      NULL, NULL, gst_udp_marshal_BOXED__STRING_INT, G_TYPE_VALUE_ARRAY, 2,
      G_TYPE_STRING, G_TYPE_INT);
  /**
   * GstMultiUDPSink::client-added:
   * @gstmultiudpsink: the sink emitting the signal
   * @host: the hostname/IP address of the added client
   * @port: the port of the added client
   *
   * Signal emited when a new client is added to the list of
   * clients.
   */
  gst_multiudpsink_signals[SIGNAL_CLIENT_ADDED] =
      g_signal_new ("client-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstMultiUDPSinkClass, client_added),
      NULL, NULL, gst_udp_marshal_VOID__STRING_INT, G_TYPE_NONE, 2,
      G_TYPE_STRING, G_TYPE_INT);
  /**
   * GstMultiUDPSink::client-removed:
   * @gstmultiudpsink: the sink emitting the signal
   * @host: the hostname/IP address of the removed client
   * @port: the port of the removed client
   *
   * Signal emited when a client is removed from the list of
   * clients.
   */
  gst_multiudpsink_signals[SIGNAL_CLIENT_REMOVED] =
      g_signal_new ("client-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstMultiUDPSinkClass,
          client_removed), NULL, NULL, gst_udp_marshal_VOID__STRING_INT,
      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_BYTES_TO_SERVE,
      g_param_spec_uint64 ("bytes-to-serve", "Bytes to serve",
          "Number of bytes received to serve to clients", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_BYTES_SERVED,
      g_param_spec_uint64 ("bytes-served", "Bytes served",
          "Total number of bytes send to all clients", 0, G_MAXUINT64, 0,
          G_PARAM_READABLE));
  g_object_class_install_property (gobject_class, PROP_SOCKFD,
      g_param_spec_int ("sockfd", "Socket Handle",
          "Socket to use for UDP sending. (-1 == allocate)",
          -1, G_MAXINT, DEFAULT_SOCKFD, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_CLOSEFD,
      g_param_spec_boolean ("closefd", "Close sockfd",
          "Close sockfd if passed as property on state change",
          DEFAULT_CLOSEFD, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_SOCK,
      g_param_spec_int ("sock", "Socket Handle",
          "Socket currently in use for UDP sending. (-1 == no socket)",
          -1, G_MAXINT, DEFAULT_SOCK, G_PARAM_READABLE));
  g_object_class_install_property (gobject_class, PROP_CLIENTS,
      g_param_spec_string ("clients", "Clients",
          "A comma separated list of host:port pairs with destinations",
          DEFAULT_CLIENTS, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_AUTO_MULTICAST,
      g_param_spec_boolean ("auto-multicast",
          "Automatically join/leave multicast groups",
          "Automatically join/leave the multicast groups, FALSE means user"
          " has to do it himself", DEFAULT_AUTO_MULTICAST, G_PARAM_READWRITE));

  gstelement_class->change_state = gst_multiudpsink_change_state;

  gstbasesink_class->render = gst_multiudpsink_render;
  klass->add = gst_multiudpsink_add;
  klass->remove = gst_multiudpsink_remove;
  klass->clear = gst_multiudpsink_clear;
  klass->get_stats = gst_multiudpsink_get_stats;

  GST_DEBUG_CATEGORY_INIT (multiudpsink_debug, "multiudpsink", 0, "UDP sink");
}


static void
gst_multiudpsink_init (GstMultiUDPSink * sink)
{
  WSA_STARTUP (sink);

  sink->client_lock = g_mutex_new ();
  sink->sock = -1;
  sink->sockfd = DEFAULT_SOCKFD;
  sink->closefd = DEFAULT_CLOSEFD;
  sink->externalfd = (sink->sockfd != -1);
  sink->auto_multicast = DEFAULT_AUTO_MULTICAST;
}

static void
gst_multiudpsink_finalize (GObject * object)
{
  GstMultiUDPSink *sink;

  sink = GST_MULTIUDPSINK (object);

  g_list_foreach (sink->clients, (GFunc) free_client, NULL);
  g_list_free (sink->clients);

  g_mutex_free (sink->client_lock);

  WSA_CLEANUP (object);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstFlowReturn
gst_multiudpsink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstMultiUDPSink *sink;
  gint ret, size, num = 0;
  guint8 *data;
  GList *clients;

  sink = GST_MULTIUDPSINK (bsink);

  size = GST_BUFFER_SIZE (buffer);
  data = GST_BUFFER_DATA (buffer);

  sink->bytes_to_serve += size;

  /* grab lock while iterating and sending to clients, this shuld be
   * fast as UDP never blocks */
  g_mutex_lock (sink->client_lock);
  GST_LOG_OBJECT (bsink, "about to send %d bytes", size);

  for (clients = sink->clients; clients; clients = g_list_next (clients)) {
    GstUDPClient *client;

    client = (GstUDPClient *) clients->data;
    num++;
    GST_LOG_OBJECT (sink, "sending %d bytes to client %p", size, client);

    while (TRUE) {
      ret = sendto (*client->sock, data, size, 0,
          (struct sockaddr *) &client->theiraddr, sizeof (client->theiraddr));

      if (ret < 0) {
        if (errno != EINTR && errno != EAGAIN) {
          goto send_error;
        }
      } else {
        client->bytes_sent += ret;
        client->packets_sent++;
        sink->bytes_served += ret;
        break;
      }
    }
  }
  g_mutex_unlock (sink->client_lock);

  GST_LOG_OBJECT (sink, "sent %d bytes to %d clients", size, num);

  return GST_FLOW_OK;

  /* ERRORS */
send_error:
  {
    /* if sendto returns an error, something is seriously wrong */
    g_mutex_unlock (sink->client_lock);
    GST_DEBUG_OBJECT (sink, "got send error %d: %s", errno, g_strerror (errno));
    GST_ELEMENT_ERROR (sink, STREAM, FAILED, (NULL),
        ("Got send error %d: %s", errno, g_strerror (errno)));
    return GST_FLOW_ERROR;
  }
}

static void
gst_multiudpsink_set_clients_string (GstMultiUDPSink * sink,
    const gchar * string)
{
  gchar **clients;
  gint i;

  clients = g_strsplit (string, ",", 0);

  g_mutex_lock (sink->client_lock);
  /* clear all existing clients */
  gst_multiudpsink_clear_internal (sink, FALSE);
  for (i = 0; clients[i]; i++) {
    gchar *host, *p;
    gint port = 0;

    host = clients[i];
    p = strstr (clients[i], ":");
    if (p != NULL) {
      *p = '\0';
      port = atoi (p + 1);
    }
    if (port != 0)
      gst_multiudpsink_add_internal (sink, host, port, FALSE);
  }
  g_mutex_unlock (sink->client_lock);

  g_strfreev (clients);
}

static gchar *
gst_multiudpsink_get_clients_string (GstMultiUDPSink * sink)
{
  GString *str;
  GList *clients;

  str = g_string_new ("");

  g_mutex_lock (sink->client_lock);
  clients = sink->clients;
  while (clients) {
    GstUDPClient *client;

    client = (GstUDPClient *) clients->data;

    clients = g_list_next (clients);

    g_string_append_printf (str, "%s:%d%s", client->host, client->port,
        (clients ? "," : ""));
  }
  g_mutex_unlock (sink->client_lock);

  return g_string_free (str, FALSE);
}

static void
gst_multiudpsink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMultiUDPSink *udpsink;

  udpsink = GST_MULTIUDPSINK (object);

  switch (prop_id) {
    case PROP_SOCKFD:
      udpsink->sockfd = g_value_get_int (value);
      GST_DEBUG_OBJECT (udpsink, "setting SOCKFD to %d", udpsink->sockfd);
      break;
    case PROP_CLOSEFD:
      udpsink->closefd = g_value_get_boolean (value);
      break;
    case PROP_CLIENTS:
      gst_multiudpsink_set_clients_string (udpsink, g_value_get_string (value));
      break;
    case PROP_AUTO_MULTICAST:
      udpsink->auto_multicast = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_multiudpsink_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstMultiUDPSink *udpsink;

  udpsink = GST_MULTIUDPSINK (object);

  switch (prop_id) {
    case PROP_BYTES_TO_SERVE:
      g_value_set_uint64 (value, udpsink->bytes_to_serve);
      break;
    case PROP_BYTES_SERVED:
      g_value_set_uint64 (value, udpsink->bytes_served);
      break;
    case PROP_SOCKFD:
      g_value_set_int (value, udpsink->sockfd);
      break;
    case PROP_CLOSEFD:
      g_value_set_boolean (value, udpsink->closefd);
      break;
    case PROP_SOCK:
      g_value_set_int (value, udpsink->sock);
      break;
    case PROP_CLIENTS:
      g_value_take_string (value,
          gst_multiudpsink_get_clients_string (udpsink));
      break;
    case PROP_AUTO_MULTICAST:
      g_value_set_boolean (value, udpsink->auto_multicast);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
join_multicast (GstUDPClient * client)
{
  unsigned char ttl = 64;
  unsigned char loop = 1;

  /* Joining the multicast group */
  /* FIXME, can we use multicast and unicast over the same
   * socket? if not, search for socket of this multicast group or
   * create a new one. */
  if (setsockopt (*(client->sock), IPPROTO_IP, IP_ADD_MEMBERSHIP,
          &(client->multi_addr), sizeof (client->multi_addr)) < 0)
    perror ("setsockopt IP_ADD_MEMBERSHIP\n");
  if (setsockopt (*(client->sock), IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
          sizeof (ttl)) < 0)
    perror ("setsockopt IP_MULTICAST_TTL\n");
  if (setsockopt (*(client->sock), IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
          sizeof (loop)) < 0)
    perror ("setsockopt IP_MULTICAST_LOOP\n");
}

static void
leave_multicast (GstUDPClient * client)
{
  if (setsockopt (*(client->sock), IPPROTO_IP, IP_DROP_MEMBERSHIP,
          &(client->multi_addr), sizeof (client->multi_addr)) < 0)
    GST_WARNING ("setsockopt IP_DROP_MEMBERSHIP failed '%s'",
        g_strerror (errno));
}

/* create a socket for sending to remote machine */
static gboolean
gst_multiudpsink_init_send (GstMultiUDPSink * sink)
{
  guint bc_val;
  gint ret;
  GList *clients;
  GstUDPClient *client;

  if (sink->sockfd == -1) {
    /* create sender socket */
    if ((sink->sock = socket (AF_INET, SOCK_DGRAM, 0)) == -1)
      goto no_socket;

    sink->externalfd = FALSE;
  } else {
    /* we use the configured socket */
    sink->sock = sink->sockfd;
    sink->externalfd = TRUE;
  }

  bc_val = 1;
  if ((ret =
          setsockopt (sink->sock, SOL_SOCKET, SO_BROADCAST, &bc_val,
              sizeof (bc_val))) < 0)
    goto no_broadcast;

  sink->bytes_to_serve = 0;
  sink->bytes_served = 0;

  /* look for multicast clients and join multicast groups approptiately */

  for (clients = sink->clients; clients; clients = g_list_next (clients)) {
    client = (GstUDPClient *) clients->data;
    if (client->multi_addr.imr_multiaddr.s_addr && sink->auto_multicast)
      join_multicast (client);
  }
  return TRUE;

  /* ERRORS */
no_socket:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, FAILED, (NULL),
        ("Could not create socket (%d): %s", errno, g_strerror (errno)));
    return FALSE;
  }
no_broadcast:
  {
    CLOSE_IF_REQUESTED (sink);
    GST_ELEMENT_ERROR (sink, RESOURCE, SETTINGS, (NULL),
        ("Could not set broadcast socket option (%d): %s", errno,
            g_strerror (errno)));
    return FALSE;
  }
}

static void
gst_multiudpsink_close (GstMultiUDPSink * sink)
{
  CLOSE_IF_REQUESTED (sink);
}

static void
gst_multiudpsink_add_internal (GstMultiUDPSink * sink, const gchar * host,
    gint port, gboolean lock)
{
  struct hostent *he;
  struct in_addr addr;
  GstUDPClient *client;
  GTimeVal now;

  GST_DEBUG_OBJECT (sink, "adding client on host %s, port %d", host, port);
  client = g_new0 (GstUDPClient, 1);
  client->host = g_strdup (host);
  client->port = port;
  client->sock = &sink->sock;

  memset (&client->theiraddr, 0, sizeof (client->theiraddr));
  memset (&client->multi_addr, 0, sizeof (client->multi_addr));
  client->theiraddr.sin_family = AF_INET;       /* host byte order */
  client->theiraddr.sin_port = htons (port);    /* short, network byte order */

  g_get_current_time (&now);
  client->connect_time = GST_TIMEVAL_TO_TIME (now);

  /* if its an IP address */
  if (inet_aton (host, &addr)) {
    /* check if its a multicast address */
    if ((ntohl (addr.s_addr) & 0xf0000000) == 0xe0000000) {
      GST_DEBUG_OBJECT (sink, "multicast address detected");
      client->multi_addr.imr_multiaddr.s_addr = addr.s_addr;
      client->multi_addr.imr_interface.s_addr = INADDR_ANY;

      client->theiraddr.sin_addr = client->multi_addr.imr_multiaddr;

    } else {
      GST_DEBUG_OBJECT (sink, "normal address detected");
      client->theiraddr.sin_addr = *((struct in_addr *) &addr);
    }
    /* if init_send has already been called, set sockopts for multicast */
    if (*client->sock > 0 && client->multi_addr.imr_multiaddr.s_addr &&
        sink->auto_multicast)
      join_multicast (client);
  }
  /* we dont need to lookup for localhost */
  else if (strcmp (host, "localhost") == 0 && inet_aton ("127.0.0.1", &addr)) {
    client->theiraddr.sin_addr = *((struct in_addr *) &addr);
  }
  /* if its a hostname */
  else if ((he = gethostbyname (host))) {
    client->theiraddr.sin_addr = *((struct in_addr *) he->h_addr);
  } else {
    goto host_error;
  }

  if (lock)
    g_mutex_lock (sink->client_lock);
  sink->clients = g_list_prepend (sink->clients, client);
  if (lock)
    g_mutex_unlock (sink->client_lock);

  g_signal_emit (G_OBJECT (sink),
      gst_multiudpsink_signals[SIGNAL_CLIENT_ADDED], 0, host, port);

  return;

  /* ERRORS */
host_error:
  {
    GST_WARNING_OBJECT (sink, "hostname lookup error?");
    g_free (client->host);
    g_free (client);
    return;
  }
}

void
gst_multiudpsink_add (GstMultiUDPSink * sink, const gchar * host, gint port)
{
  gst_multiudpsink_add_internal (sink, host, port, TRUE);
}

static gint
client_compare (GstUDPClient * a, GstUDPClient * b)
{
  if ((a->port == b->port) && (strcmp (a->host, b->host) == 0))
    return 0;

  return 1;
}

static void
free_client (GstUDPClient * client)
{
  g_free (client->host);
  g_free (client);
}

void
gst_multiudpsink_remove (GstMultiUDPSink * sink, const gchar * host, gint port)
{
  GList *find;
  GstUDPClient udpclient;
  GstUDPClient *client;
  GTimeVal now;

  udpclient.host = (gchar *) host;
  udpclient.port = port;

  g_mutex_lock (sink->client_lock);
  find = g_list_find_custom (sink->clients, &udpclient,
      (GCompareFunc) client_compare);
  if (!find)
    goto not_found;

  GST_DEBUG_OBJECT (sink, "remove client with host %s, port %d", host, port);

  client = (GstUDPClient *) find->data;

  g_get_current_time (&now);
  client->disconnect_time = GST_TIMEVAL_TO_TIME (now);

  if (*(client->sock) != -1 && client->multi_addr.imr_multiaddr.s_addr
      && sink->auto_multicast)
    leave_multicast (client);

  /* Unlock to emit signal before we delete the actual client */
  g_mutex_unlock (sink->client_lock);
  g_signal_emit (G_OBJECT (sink),
      gst_multiudpsink_signals[SIGNAL_CLIENT_REMOVED], 0, host, port);
  g_mutex_lock (sink->client_lock);

  sink->clients = g_list_delete_link (sink->clients, find);

  free_client (client);
  g_mutex_unlock (sink->client_lock);

  return;

  /* ERRORS */
not_found:
  {
    g_mutex_unlock (sink->client_lock);
    GST_WARNING_OBJECT (sink, "client at host %s, port %d not found",
        host, port);
    return;
  }
}

static void
gst_multiudpsink_clear_internal (GstMultiUDPSink * sink, gboolean lock)
{
  GST_DEBUG_OBJECT (sink, "clearing");
  /* we only need to remove the client structure, there is no additional
   * socket or anything to free for UDP */
  if (lock)
    g_mutex_lock (sink->client_lock);
  g_list_foreach (sink->clients, (GFunc) free_client, sink);
  g_list_free (sink->clients);
  sink->clients = NULL;
  if (lock)
    g_mutex_unlock (sink->client_lock);
}

void
gst_multiudpsink_clear (GstMultiUDPSink * sink)
{
  gst_multiudpsink_clear_internal (sink, TRUE);
}

GValueArray *
gst_multiudpsink_get_stats (GstMultiUDPSink * sink, const gchar * host,
    gint port)
{
  GstUDPClient *client;
  GValueArray *result = NULL;
  GstUDPClient udpclient;
  GList *find;
  GValue value = { 0 };

  udpclient.host = (gchar *) host;
  udpclient.port = port;

  g_mutex_lock (sink->client_lock);

  find = g_list_find_custom (sink->clients, &udpclient,
      (GCompareFunc) client_compare);
  if (!find)
    goto not_found;

  GST_DEBUG_OBJECT (sink, "stats for client with host %s, port %d", host, port);

  client = (GstUDPClient *) find->data;

  /* Result is a value array of (bytes_sent, packets_sent,
   * connect_time, disconnect_time), all as uint64 */
  result = g_value_array_new (4);

  g_value_init (&value, G_TYPE_UINT64);
  g_value_set_uint64 (&value, client->bytes_sent);
  result = g_value_array_append (result, &value);
  g_value_unset (&value);

  g_value_init (&value, G_TYPE_UINT64);
  g_value_set_uint64 (&value, client->packets_sent);
  result = g_value_array_append (result, &value);
  g_value_unset (&value);

  g_value_init (&value, G_TYPE_UINT64);
  g_value_set_uint64 (&value, client->connect_time);
  result = g_value_array_append (result, &value);
  g_value_unset (&value);

  g_value_init (&value, G_TYPE_UINT64);
  g_value_set_uint64 (&value, client->disconnect_time);
  result = g_value_array_append (result, &value);
  g_value_unset (&value);

  g_mutex_unlock (sink->client_lock);

  return result;

  /* ERRORS */
not_found:
  {
    g_mutex_unlock (sink->client_lock);
    GST_WARNING_OBJECT (sink, "client with host %s, port %d not found",
        host, port);
    /* Apparently (see comment in gstmultifdsink.c) returning NULL from here may
     * confuse/break python bindings */
    return g_value_array_new (0);
  }
}

static GstStateChangeReturn
gst_multiudpsink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstMultiUDPSink *sink;

  sink = GST_MULTIUDPSINK (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (!gst_multiudpsink_init_send (sink))
        goto no_init;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_multiudpsink_close (sink);
      break;
    default:
      break;
  }
  return ret;

  /* ERRORS */
no_init:
  {
    /* _init_send() posted specific error already */
    return GST_STATE_CHANGE_FAILURE;
  }
}
