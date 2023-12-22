#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

int main()
{
    int listen_sockfd, send_sockfd;
    struct sockaddr_in server_addr, client_addr_from, client_addr_to;
    struct packet buffer;
    socklen_t addr_size = sizeof(client_addr_from);
    int expected_seq_num = 1;
    int recv_len;
    struct packet ack_pkt;

    // Create a UDP socket for sending
    send_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (send_sockfd < 0)
    {
        perror("Could not create send socket");
        return 1;
    }

    // Create a UDP socket for listening
    listen_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (listen_sockfd < 0)
    {
        perror("Could not create listen socket");
        return 1;
    }

    // Configure the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the listen socket to the server address
    if (bind(listen_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(listen_sockfd);
        return 1;
    }

    // Configure the client address structure to which we will send ACKs
    memset(&client_addr_to, 0, sizeof(client_addr_to));
    client_addr_to.sin_family = AF_INET;
    client_addr_to.sin_addr.s_addr = inet_addr(LOCAL_HOST);
    client_addr_to.sin_port = htons(CLIENT_PORT_TO);

    // Open the target file for writing (always write to output.txt)
    FILE *fp = fopen("output.txt", "wb");
    if (fp == NULL)
    {
        perror("Failed to open file");
        return 1;
    }

    // initialize out of order buffer
    struct packet out_of_order[BUFFER_SIZE];
    for (int i = 0; i < BUFFER_SIZE; i++) {
        struct packet o_pkt;
        build_packet(&o_pkt, 0, 0, 0, 0, 0, "");
        out_of_order[i] = o_pkt;
    }

    int highest_sent_ack = 0;
    int last_ack = 0;
    int nothing_recieved = 0;
    bool client_done = false;
    bool server_done = false;

    while (1)
    {
        recv_len = recvfrom(listen_sockfd, &buffer, sizeof(struct packet), 0, (struct sockaddr *)&client_addr_from, &addr_size);
        printRecv(&buffer);

        if (recv_len > 0)
        {
            nothing_recieved = 0;
            if (buffer.seqnum == 0) {
                client_done = true;
            }

            if (server_done && client_done) {
                break;
            }

            if (buffer.last) {
                last_ack = buffer.seqnum;
            }

            if (buffer.seqnum == expected_seq_num)
            {
                // write to output and prepare to send ack
                fwrite(buffer.payload, 1, buffer.length, fp);
                expected_seq_num++;

                // process previously received out-of-order pkts
                bool missing_pkt = false;
                if (highest_sent_ack >= expected_seq_num) {
                    // write buffer to output and update expected seq num
                    int starting_index = expected_seq_num % BUFFER_SIZE;
                    for (int i = starting_index; i < BUFFER_SIZE; i++) {
                        if (out_of_order[i].seqnum == expected_seq_num) {
                            expected_seq_num++;
                            fwrite(out_of_order[i].payload, 1, out_of_order[i].length, fp);
                            out_of_order[i].seqnum = 0;
                        }
                        else {
                            missing_pkt = true;
                            break;
                        }
                    }
                    if (!missing_pkt) {
                        for (int i = 0; i < starting_index; i++) {
                            if (out_of_order[i].seqnum == expected_seq_num) {
                                expected_seq_num++;
                                fwrite(out_of_order[i].payload, 1, out_of_order[i].length, fp);
                                out_of_order[i].seqnum = 0;
                            }
                            else {
                                break;
                            }
                        }
                    }
                }

                // send ack
                build_packet(&ack_pkt, buffer.seqnum, buffer.acknum, buffer.last, 1, 0, "");
                sendto(send_sockfd, &ack_pkt, sizeof(struct packet), 0, (struct sockaddr *)&client_addr_to, sizeof(client_addr_to));
                printSend(&ack_pkt, 0);
                if (ack_pkt.acknum > highest_sent_ack) {
                    highest_sent_ack = ack_pkt.acknum;
                }
            }
            else
            {
                // store recieved packet in a buffer to be written to output later
                out_of_order[buffer.seqnum % BUFFER_SIZE] = buffer;

                build_packet(&ack_pkt, buffer.seqnum, buffer.acknum, buffer.last, 1, 0, "");
                sendto(send_sockfd, &ack_pkt, sizeof(struct packet), 0, (struct sockaddr *)&client_addr_to, sizeof(client_addr_to));
                printSend(&ack_pkt, 0);
                if (ack_pkt.acknum > highest_sent_ack) {
                    highest_sent_ack = ack_pkt.acknum;
                }
            }
        }
        else
        {
            nothing_recieved++;
        }
        if (nothing_recieved == 3) {
            break;
        }
        if (last_ack > 0) {
            if (expected_seq_num == last_ack) {
                fwrite(out_of_order[expected_seq_num % BUFFER_SIZE].payload, 1, out_of_order[expected_seq_num % BUFFER_SIZE].length, fp);
                expected_seq_num++;
            }
            if (expected_seq_num >= last_ack) {
                server_done = true;
                build_packet(&ack_pkt, 0, 0, 0, 1, 0, "");
                sendto(send_sockfd, &ack_pkt, sizeof(struct packet), 0, (struct sockaddr *)&client_addr_to, sizeof(client_addr_to));
                printSend(&ack_pkt, 0);
            }
        }
    }

    fclose(fp);
    close(listen_sockfd);
    close(send_sockfd);
    return 0;
}
