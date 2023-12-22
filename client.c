#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "utils.h"

int main(int argc, char *argv[])
{
    int listen_sockfd, send_sockfd;
    struct sockaddr_in client_addr, server_addr_to, server_addr_from;
    socklen_t addr_size = sizeof(server_addr_to);
    struct timeval tv;
    struct packet pkt;
    struct packet ack_pkt;
    char buffer[PAYLOAD_SIZE];
    unsigned short seq_num = 1;
    unsigned short ack_num = 1;

    // read filename from command line argument
    if (argc != 2)
    {
        printf("Usage: ./client <filename>\n");
        return 1;
    }
    char *filename = argv[1];

    // Create a UDP socket for listening
    listen_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_sockfd < 0)
    {
        perror("Could not create listen socket");
        return 1;
    }

    // Create a UDP socket for sending
    send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        perror("Could not create send socket");
        return 1;
    }

    // Configure the server address structure to which we will send data
    memset(&server_addr_to, 0, sizeof(server_addr_to));
    server_addr_to.sin_family = AF_INET;
    server_addr_to.sin_port = htons(SERVER_PORT_TO);
    server_addr_to.sin_addr.s_addr = inet_addr(SERVER_IP);

    // Configure the client address structure
    memset(&client_addr, 0, sizeof(client_addr));
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(CLIENT_PORT);
    client_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the listen socket to the client address
    if (bind(listen_sockfd, (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0)
    {
        perror("Bind failed");
        close(listen_sockfd);
        return 1;
    }

    // Open file for reading
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        perror("Error opening file");
        close(listen_sockfd);
        close(send_sockfd);
        return 1;
    }

    unsigned short cwnd = 1;
    unsigned short in_cwnd = 1;
    unsigned short ssh = WINDOW_SIZE;
    unsigned short mss = 1;
    unsigned short expected_ack_num = 1;
    unsigned short last_pkt = 0;
    bool sent_last = false;
    bool fastretx = false;
    bool client_done = false;
    bool server_done = false;

    char acks_rcv[BUFFER_SIZE] = {0};

    // initialize cwnd buffer
    struct packet cwnd_buf[BUFFER_SIZE];
    for (int i = 0; i < BUFFER_SIZE; i++) {
        struct packet o_pkt;
        build_packet(&o_pkt, 0, 0, 0, 0, 0, "");
        cwnd_buf[i] = o_pkt;
    }

    // Read from file and send first packet
    size_t bytes_read = fread(buffer, 1, PAYLOAD_SIZE, fp);
    build_packet(&pkt, seq_num, ack_num, bytes_read < PAYLOAD_SIZE, 0, bytes_read, buffer);
    cwnd_buf[pkt.seqnum % BUFFER_SIZE] = pkt;
    sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
    printSend(&pkt, 0);

    while (1)
    {
        // Wait for ACK with timeout
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_sockfd, &read_fds);
        tv.tv_sec = TIMEOUT;
        tv.tv_usec = 0;

        int select_result = select(listen_sockfd + 1, &read_fds, NULL, NULL, &tv);
        if (select_result == -1)
        {
            // Error occurred in select()
            perror("select");
            break;
        }
        else if (select_result == 0)
        {
            if (client_done && server_done) {
                break;
            }

            if (last_pkt > 0 && expected_ack_num >= last_pkt) {
                client_done = true;
                build_packet(&pkt, 0, 0, 0, 1, 0, "");
                sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
                printSend(&pkt, 0);
                continue;
            }

            // retransmission timeout       
            if ((cwnd / 2) > WINDOW_SIZE) {
                ssh = cwnd / 2;
            }
            else {
                ssh = WINDOW_SIZE;
            }
            cwnd = 1;
            in_cwnd = 1;
            mss = 1;
            pkt = cwnd_buf[expected_ack_num % BUFFER_SIZE];
            sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
            printSend(&pkt, 1);
            if (pkt.last == 1) {
                sent_last = true;
            }
            continue;
        }
        else
        {
            recvfrom(listen_sockfd, &ack_pkt, sizeof(struct packet), 0, (struct sockaddr *)&server_addr_from, &addr_size);
            printRecv(&ack_pkt);

            if (ack_pkt.seqnum == 0) {
                server_done = true;
            }

            if (client_done && server_done) {
                break;
            }

            if (ack_pkt.last) {
                last_pkt = ack_pkt.seqnum;
            }

            int rec_ack_num = ack_pkt.acknum;

            if (rec_ack_num > expected_ack_num) {
                acks_rcv[rec_ack_num % BUFFER_SIZE] = 1;
            }

            if (rec_ack_num == expected_ack_num)
            {
                expected_ack_num++;
                in_cwnd--;

                while (acks_rcv[expected_ack_num % BUFFER_SIZE] == 1) {
                    acks_rcv[expected_ack_num % BUFFER_SIZE] = 0;
                    expected_ack_num++;
                    in_cwnd--;
                }

                if (sent_last) {
                    continue;
                }

                if (fastretx) {
                    // fast recovery with new ack
                    cwnd = ssh;
                    in_cwnd = ssh;
                    fastretx = false;
                    seq_num++;
                    ack_num = seq_num;
                    bytes_read = fread(buffer, 1, PAYLOAD_SIZE, fp);
                    build_packet(&pkt, seq_num, ack_num, bytes_read < PAYLOAD_SIZE, 0, bytes_read, buffer);
                    cwnd_buf[pkt.seqnum % BUFFER_SIZE] = pkt;
                    sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
                    printSend(&pkt, 0);
                    if (pkt.last == 1) {
                        sent_last = true;
                    }
                }

                if (cwnd <= ssh) {
                    // slow start
                    cwnd++;
                }
                else {
                    // congestion avoidance
                    if ((mss / cwnd) == 1) {
                        cwnd++;
                        mss = 1;
                    }
                    else {
                        mss++;
                    }
                }
                for (; in_cwnd < cwnd && !sent_last; in_cwnd++) {
                    seq_num++;
                    ack_num = seq_num;
                    bytes_read = fread(buffer, 1, PAYLOAD_SIZE, fp);
                    build_packet(&pkt, seq_num, ack_num, bytes_read < PAYLOAD_SIZE, 0, bytes_read, buffer);
                    cwnd_buf[pkt.seqnum % BUFFER_SIZE] = pkt;
                    sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
                    printSend(&pkt, 0);
                    if (pkt.last == 1) {
                        sent_last = true;
                    }
                }
            }
            else {
                if (!fastretx) {
                    // fast retransmit
                    if ((cwnd / 2) > WINDOW_SIZE) {
                        ssh = cwnd / 2;
                    }
                    else {
                        ssh = WINDOW_SIZE;
                    }
                    cwnd = ssh + 3;
                    in_cwnd++;
                    mss = 1;
                    fastretx = true;
                    pkt = cwnd_buf[expected_ack_num % BUFFER_SIZE];
                    sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
                    printSend(&pkt, 1);
                    if (pkt.last == 1) {
                        sent_last = true;
                    }
                }
            }
        }

        if (last_pkt > 0) {
            if (expected_ack_num >= last_pkt) {
                client_done = true;
                build_packet(&pkt, 0, 0, 0, 1, 0, "");
                sendto(send_sockfd, &pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server_addr_to, sizeof(server_addr_to));
                printSend(&pkt, 0);
            }
        }
    }

    fclose(fp);
    close(listen_sockfd);
    close(send_sockfd);
    return 0;
}
