/*
 * wol - wake on lan client
 *
 * $Id: net.c,v 1.7 2004/04/18 09:34:21 wol Exp $
 *
 * Copyright (C) 2000,2001,2002,2003,2004 Thomas Krennwallner <krennwallner@aon.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */



#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */


#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <error.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_LINUX_IF_PACKET_H
#include <linux/if_packet.h>
#endif

#ifdef HAVE_NET_ETHERNET_H
#include <net/ethernet.h>
#endif

#include "net.h"
#include "wol.h"


static int
net_resolv (const char *hostname, struct in_addr *sin_addr)
{
  struct hostent *hent;

  hent = gethostbyname (hostname);
  if (hent == NULL)
    {
      errno = EINVAL;
      return -1;
    }

  memcpy ((void *) sin_addr, (const void *) hent->h_addr, (size_t) hent->h_length);

  return 0;
}


static int
net_ifaddr4 (const char *ifname, struct in_addr *addr)
{
  struct ifaddrs *ifaddr;
  struct ifaddrs *ifa;

  if (ifname == NULL || addr == NULL)
    {
      errno = EINVAL;
      return -1;
    }

  if (getifaddrs (&ifaddr) != 0)
    {
      return -1;
    }

  for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
      if (ifa->ifa_addr == NULL)
        continue;

      if (ifa->ifa_addr->sa_family != AF_INET)
        continue;

      if (strcmp (ifa->ifa_name, ifname) != 0)
        continue;

      *addr = ((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;
      freeifaddrs (ifaddr);
      return 0;
    }

  freeifaddrs (ifaddr);
  errno = ENOENT;
  return -1;
}


static int
net_configure_udp_interface (int sockfd, const char *interface_name)
{
  int configured = 0;

  if (interface_name == NULL)
    {
      return 0;
    }

  /* If the argument is an IPv4 address, bind to it directly. */
  {
    struct in_addr addr;
    if (inet_aton (interface_name, &addr) != 0)
      {
        struct sockaddr_in local;
        memset (&local, 0, sizeof (local));
        local.sin_family = AF_INET;
        local.sin_addr = addr;
        local.sin_port = htons (0);
        if (bind (sockfd, (const struct sockaddr *) &local, sizeof (local)) == 0)
          {
            return 0;
          }
        return -1;
      }
  }

  /* Prefer an OS-level "bind to interface" when available. */
#ifdef IP_BOUND_IF
  {
    unsigned int idx = if_nametoindex (interface_name);
    if (idx != 0)
      {
        if (setsockopt (sockfd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof (idx)) == 0)
          {
            configured = 1;
          }
      }
  }
#endif

  /* Fallback: bind the socket's source address to the interface's IPv4. */
  {
    struct in_addr addr;
    if (net_ifaddr4 (interface_name, &addr) == 0)
      {
        struct sockaddr_in local;
        memset (&local, 0, sizeof (local));
        local.sin_family = AF_INET;
        local.sin_addr = addr;
        local.sin_port = htons (0);
        if (bind (sockfd, (const struct sockaddr *) &local, sizeof (local)) == 0)
          {
            configured = 1;
          }
      }
  }

  if (!configured)
    {
      errno = EINVAL;
      return -1;
    }

  return 0;
}



int
net_close (int socket)
{
  if (close (socket))
    {
      perror ("Couldn't close socket");
      return -1;
    }

  return 0;
}



int
raw_open (void)
{
#if !HAVE_DECL_PF_PACKET
  errno = ENOTSUP;
  return -1;
#else
  int optval;
  int sockfd;

  sockfd = socket (PF_PACKET, SOCK_RAW, 0);
  if (sockfd < 0)
    {
      if (errno == EPERM)
	{
	  error (0, 0, "No root privileges");
	}
      else
	{
	  perror ("socket() failed");
	}

      return -1;
    }

  optval = 1;

  if (setsockopt (sockfd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof (optval)))
    {
      perror ("setsockopt() failed");
      close (sockfd);
      return -1;
    }

  return sockfd;
#endif
}

int
udp_open (const char *interface_name)
{
  int optval;
  int sockfd;

  sockfd = socket (PF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      perror ("socket() failed");
      return -1;
    }

  optval = 1;

  if (setsockopt (sockfd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof (optval)))
    {
      perror ("setsockopt() failed");
      close (sockfd);
      return -1;
    }

  if (net_configure_udp_interface (sockfd, interface_name) != 0)
    {
      error (0, errno, _("Cannot use interface '%s'"), interface_name);
      close (sockfd);
      return -1;
    }

  return sockfd;
}



int
tcp_open (const char *ip_str,
	  unsigned int port)
{
  int sockfd;
  struct sockaddr_in toaddr;

  if (ip_str == NULL)
    {
      return -1;
    }
	
  memset (&toaddr, 0, sizeof (struct sockaddr_in));

  if (net_resolv (ip_str, &toaddr.sin_addr))
    {
      error (0, 0, _("Invalid IP address given: %s"), strerror (errno));
      return -1;
    }

  toaddr.sin_family = AF_INET;
  toaddr.sin_port = htons (port);

  sockfd = socket (PF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    {
      perror ("socket() failed");
      return -1;
    }

  if (connect (sockfd, (const struct sockaddr *) &toaddr, sizeof (toaddr)))
    {
      error (0, 0, _("Couldn't connect to %s:%hu: %s"), ip_str, port, strerror (errno));
      close (sockfd);
      return -1;
    }

  return sockfd;
}



ssize_t 
udp_send (int socket,
	  const char *ip_str,
	  unsigned short int port,
	  const void *buf,
	  size_t len)
{
  struct sockaddr_in toaddr;
  ssize_t sendret;


  if (ip_str == NULL || buf == NULL)
    {
      return -1;
    }
	
  memset (&toaddr, 0, sizeof (struct sockaddr_in));

  if (net_resolv (ip_str, &toaddr.sin_addr))
    {
      error (0, 0, _("Invalid IP address given: %s"), strerror (errno));
      return -1;
    }

  toaddr.sin_family = AF_INET;
  toaddr.sin_port = htons (port);

  /* keep on sending and check for possible errors */
  sendret = sendto (socket, buf, len, 0, (struct sockaddr *) &toaddr,
		    sizeof (struct sockaddr_in));

  return sendret == -1 ? sendret : 0;
}



ssize_t
tcp_send (int sock,
	  const void *buf,
	  size_t len)
{
  return send (sock, buf, len, 0);
}



ssize_t
tcp_recv (int sock,
	  void *buf,
	  size_t len)
{
  return recv (sock, buf, len, 0);
}
