/****************************************************************************
 * apps/external/k1_udp_echo/k1_udp_echo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define K1_UDP_ECHO_BUFFER_SIZE 1472
#define K1_UDP_ECHO_TIMEOUT_SEC 3

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct sockaddr_in local;
  struct sockaddr_in peer;
  struct timeval timeout;
  FAR uint8_t *buffer;
  socklen_t peerlen;
  int sockfd;
  int packet;
  int ret;

  (void)argc;
  (void)argv;

  buffer = malloc(K1_UDP_ECHO_BUFFER_SIZE);
  if (buffer == NULL)
    {
      printf("k1_udpecho: FAIL buffer allocation\n");
      return EXIT_FAILURE;
    }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    {
      printf("k1_udpecho: FAIL socket: %d\n", errno);
      free(buffer);
      return EXIT_FAILURE;
    }

  memset(&local, 0, sizeof(local));
  local.sin_family      = AF_INET;
  local.sin_port        = htons(CONFIG_K1_UDP_ECHO_PORT);
  local.sin_addr.s_addr = htonl(INADDR_ANY);

  ret = bind(sockfd, (FAR struct sockaddr *)&local, sizeof(local));
  if (ret < 0)
    {
      printf("k1_udpecho: FAIL bind: %d\n", errno);
      close(sockfd);
      free(buffer);
      return EXIT_FAILURE;
    }

  timeout.tv_sec  = K1_UDP_ECHO_TIMEOUT_SEC;
  timeout.tv_usec = 0;
  ret = setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout));
  if (ret < 0)
    {
      printf("k1_udpecho: FAIL setsockopt: %d\n", errno);
      close(sockfd);
      free(buffer);
      return EXIT_FAILURE;
    }

  printf("k1_udpecho: listening UDP/%d for %d datagrams\n",
         CONFIG_K1_UDP_ECHO_PORT, CONFIG_K1_UDP_ECHO_PACKETS);

  for (packet = 0; packet < CONFIG_K1_UDP_ECHO_PACKETS; packet++)
    {
      ssize_t received;
      ssize_t sent;
      uint32_t address;

      memset(&peer, 0, sizeof(peer));
      peerlen = sizeof(peer);
      received = recvfrom(sockfd, buffer, K1_UDP_ECHO_BUFFER_SIZE, 0,
                          (FAR struct sockaddr *)&peer, &peerlen);
      if (received < 0)
        {
          printf("k1_udpecho: FAIL receive %d/%d: %d\n", packet + 1,
                 CONFIG_K1_UDP_ECHO_PACKETS, errno);
          close(sockfd);
          free(buffer);
          return EXIT_FAILURE;
        }

      if (peer.sin_family != AF_INET || peerlen != sizeof(peer))
        {
          printf("k1_udpecho: FAIL receive %d/%d: unexpected peer\n",
                 packet + 1, CONFIG_K1_UDP_ECHO_PACKETS);
          close(sockfd);
          free(buffer);
          return EXIT_FAILURE;
        }

      sent = sendto(sockfd, buffer, received, 0,
                    (FAR const struct sockaddr *)&peer, peerlen);
      if (sent != received)
        {
          printf("k1_udpecho: FAIL send %d/%d: %d\n", packet + 1,
                 CONFIG_K1_UDP_ECHO_PACKETS,
                 sent < 0 ? errno : EIO);
          close(sockfd);
          free(buffer);
          return EXIT_FAILURE;
        }

      address = ntohl(peer.sin_addr.s_addr);
      printf("k1_udpecho: echoed %d/%d: %ld bytes to %u.%u.%u.%u:%u\n",
             packet + 1, CONFIG_K1_UDP_ECHO_PACKETS, (long)received,
             (unsigned int)((address >> 24) & 0xff),
             (unsigned int)((address >> 16) & 0xff),
             (unsigned int)((address >> 8) & 0xff),
             (unsigned int)(address & 0xff), ntohs(peer.sin_port));
    }

  close(sockfd);
  free(buffer);
  printf("k1_udpecho: PASS %d/%d datagrams echoed\n",
         CONFIG_K1_UDP_ECHO_PACKETS, CONFIG_K1_UDP_ECHO_PACKETS);
  return EXIT_SUCCESS;
}
