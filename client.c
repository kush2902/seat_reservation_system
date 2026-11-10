#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"

static int g_current_user_id = -1;
static int g_current_role = -1;

int connect_server() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr;
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, "127.0.0.1", &saddr.sin_addr);

    if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("Connect failed");
        return -1;
    }
    return sock;
}

int send_recv(uint16_t op, const void *req_body, uint32_t req_len, 
              uint16_t *resp_op, uint16_t *resp_status, void *resp_body, uint32_t max_resp) {
    int sock = connect_server();
    if (sock < 0) return -1;

    PacketHeader hdr;
    hdr.magic = htonl(PROTO_MAGIC);
    hdr.opcode = htons(op);
    hdr.status = 0;
    hdr.payload_len = htonl(req_len);

    write(sock, &hdr, sizeof(hdr));
    if (req_len > 0) write(sock, req_body, req_len);

    PacketHeader rhdr;
    if (read(sock, &rhdr, sizeof(rhdr)) != sizeof(rhdr)) {
        close(sock);
        return -1;
    }

    *resp_op = ntohs(rhdr.opcode);
    *resp_status = ntohs(rhdr.status);
    uint32_t plen = ntohl(rhdr.payload_len);

    if (plen > 0 && resp_body != NULL) {
        uint32_t to_read = (plen < max_resp) ? plen : max_resp;
        read(sock, resp_body, to_read);
    }

    close(sock);
    return 0;
}

void view_train_map(int train_id) {
    ViewTrainReqPayload req = { train_id };
    ViewTrainRespPayload resp;
    uint16_t rop, rstat;

    if (send_recv(OP_VIEW_TRAIN_REQ, &req, sizeof(req), &rop, &rstat, &resp, sizeof(resp)) == 0) {
        if (rstat == 0) {
            printf("\n--- %s (ID: %d) | Base Fare: $%.2f ---\n", resp.train_name, resp.train_id, resp.base_fare);
            printf("Seat Map ([O] = Available, [X] = Booked):\n");
            for (int i = 0; i < TOTAL_SEATS; i++) {
                printf("%02d:[%c]  ", i + 1, resp.seat_map[i] ? 'X' : 'O');
                if ((i + 1) % 6 == 0) printf("\n");
            }
            printf("\n");
        } else {
            printf("Error fetching train details: Status %d\n", rstat);
        }
    }
}

void book_seat_interactive() {
    int train_id, seat_no;
    printf("Enter Train ID (e.g. 101): "); scanf("%d", &train_id);
    printf("Enter Seat Number (1-%d): ", TOTAL_SEATS); scanf("%d", &seat_no);

    BookSeatReqPayload req = { g_current_user_id, train_id, seat_no };
    BookSeatRespPayload resp;
    uint16_t rop, rstat;

    send_recv(OP_BOOK_SEAT_REQ, &req, sizeof(req), &rop, &rstat, &resp, sizeof(resp));
    if (rstat == 0) {
        printf(">> SUCCESS: Booked Seat %d! Booking ID: %d | Total Charged: $%.2f\n",
               resp.seat_number, resp.booking_id, resp.fare_charged);
    } else if (rstat == 409) {
        printf(">> ERROR: Seat %d is already taken! Please pick another.\n", seat_no);
    } else {
        printf(">> ERROR: Reservation failed (Code: %d)\n", rstat);
    }
}

void cancel_seat_interactive() {
    int b_id;
    printf("Enter Booking ID to cancel: "); scanf("%d", &b_id);

    CancelBookingReqPayload req = { g_current_user_id, b_id };
    uint16_t rop, rstat;

    send_recv(OP_CANCEL_BOOKING_REQ, &req, sizeof(req), &rop, &rstat, NULL, 0);
    if (rstat == 0) {
        printf(">> SUCCESS: Booking #%d has been cancelled and seat released.\n", b_id);
    } else {
        printf(">> ERROR: Failed to cancel booking (Code: %d)\n", rstat);
    }
}

void update_price_interactive() {
    int train_id;
    double new_fare;
    printf("Enter Train ID: "); scanf("%d", &train_id);
    printf("Enter New Base Fare: "); scanf("%lf", &new_fare);

    UpdateFareReqPayload req = { g_current_user_id, train_id, new_fare };
    uint16_t rop, rstat;

    send_recv(OP_ADMIN_UPDATE_PRICE_REQ, &req, sizeof(req), &rop, &rstat, NULL, 0);
    if (rstat == 0) {
        printf(">> SUCCESS: Base fare updated to $%.2f.\n", new_fare);
    } else {
        printf(">> ERROR: Failed to update fare (Code: %d)\n", rstat);
    }
}

int main() {
    printf("=== Railway/Transit High-Performance Booking Engine ===\n");
    printf("1. Passenger Login (seeded: passenger1 / pass123)\n");
    printf("2. Administrator Login (seeded: admin1 / admin123)\n");
    printf("Select Choice: ");
    
    int choice;
    if (scanf("%d", &choice) != 1) return 0;

    int role = (choice == 1) ? ROLE_PASSENGER : ROLE_ADMIN;
    char username[MAX_NAME_LEN], password[MAX_NAME_LEN];
    printf("Username: "); scanf("%31s", username);
    printf("Password: "); scanf("%31s", password);

    LoginReqPayload lreq;
    lreq.role_id = role;
    strncpy(lreq.username, username, MAX_NAME_LEN);
    strncpy(lreq.password, password, MAX_NAME_LEN);

    LoginRespPayload lresp;
    uint16_t rop, rstat;

    if (send_recv(OP_LOGIN_REQ, &lreq, sizeof(lreq), &rop, &rstat, &lresp, sizeof(lresp)) != 0 || rstat != 0) {
        printf("Authentication failed (Status code: %d).\n", rstat);
        return 1;
    }

    g_current_user_id = lresp.user_id;
    g_current_role = lresp.role_id;
    printf("Login successful. Assigned User ID: %d\n", g_current_user_id);

    while (1) {
        if (g_current_role == ROLE_PASSENGER) {
            printf("\n--- Passenger Menu ---\n");
            printf("1. View Train & Seat Map\n");
            printf("2. Reserve Specific Seat\n");
            printf("3. Cancel Booking\n");
            printf("4. Exit\n");
            printf("Choice: ");
            int ch; scanf("%d", &ch);
            if (ch == 1) view_train_map(101);
            else if (ch == 2) book_seat_interactive();
            else if (ch == 3) cancel_seat_interactive();
            else break;
        } else {
            printf("\n--- Administrator Menu ---\n");
            printf("1. View Train & Seat Map\n");
            printf("2. Update Base Ticket Fare\n");
            printf("3. Exit\n");
            printf("Choice: ");
            int ch; scanf("%d", &ch);
            if (ch == 1) view_train_map(101);
            else if (ch == 2) update_price_interactive();
            else break;
        }
    }
    return 0;
}
