// range_client.c - minimal range request tester
// gcc -o range_client range_client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define HOST "127.0.0.1"
#define PORT 8080
#define PATH "/goddess.mp4"
#define RANGE_START 100000
#define RANGE_END 100200   // small range for testing

int main() {
    int sockfd;
    struct sockaddr_in servaddr;
    char request[512];
    char response[4096];
    ssize_t n;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr(HOST);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("connect"); close(sockfd); return 1;
    }

    snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Range: bytes=%d-%d\r\n"
        "Connection: close\r\n"
        "\r\n",
        PATH, HOST, RANGE_START, RANGE_END);

    printf("Request:\n%s", request);
    write(sockfd, request, strlen(request));

    // Read full response
    ssize_t total = 0;
    while ((n = read(sockfd, response + total, sizeof(response) - total - 1)) > 0) {
        total += n;
        if (total >= (ssize_t)sizeof(response) - 1) break;
    }
    response[total] = '\0';
    close(sockfd);

    printf("Received %zd bytes\n", total);
    printf("Response (first 800 chars):\n%.800s\n", response);

    // Quick check for 206
    if (strstr(response, "HTTP/1.1 206") || strstr(response, "HTTP/1.0 206")) {
        printf("SUCCESS: Got 206 Partial Content\n");
    } else if (strstr(response, "HTTP/1.1 200")) {
        printf("WARNING: Got 200 (no range served)\n");
    } else {
        printf("UNKNOWN status\n");
    }

    if (strstr(response, "Content-Range:")) {
        printf("Content-Range header present\n");
    } else {
        printf("No Content-Range header\n");
    }

    return 0;
}