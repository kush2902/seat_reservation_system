#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include "protocol.h"

#define MAX_CLIENTS 64
#define USERS_DB    "users.bin"
#define TRAINS_DB   "trains.bin"
#define BOOKINGS_DB "bookings.bin"

static int g_users_fd = -1;
static int g_trains_fd = -1;
static int g_bookings_fd = -1;

// Granular Record-Level File Locking helper
int set_record_lock(int fd, short type, off_t offset, off_t length, int wait) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;       // F_RDLCK, F_WRLCK, or F_UNLCK
    fl.l_whence = SEEK_SET;
    fl.l_start = offset;
    fl.l_len = length;      // Lock only the specific byte span

    int cmd = wait ? F_SETLKW : F_SETLK;
    if (fcntl(fd, cmd, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) return -1; // Locked by another process
        perror("fcntl lock error");
        return -2;
    }
    return 0;
}

// Database Seeding
void init_databases() {
    g_users_fd = open(USERS_DB, O_RDWR | O_CREAT, 0666);
    g_trains_fd = open(TRAINS_DB, O_RDWR | O_CREAT, 0666);
    g_bookings_fd = open(BOOKINGS_DB, O_RDWR | O_CREAT, 0666);

    if (g_users_fd < 0 || g_trains_fd < 0 || g_bookings_fd < 0) {
        perror("Failed to open DB files");
        exit(1);
    }

    // Seed Users if empty
    if (lseek(g_users_fd, 0, SEEK_END) == 0) {
        UserRecord default_users[] = {
            {1, ROLE_PASSENGER, "passenger1", "pass123"},
            {2, ROLE_PASSENGER, "passenger2", "pass123"},
            {3, ROLE_ADMIN,     "admin1",     "admin123"}
        };
        write(g_users_fd, default_users, sizeof(default_users));
    }

    // Seed Train if empty
    if (lseek(g_trains_fd, 0, SEEK_END) == 0) {
        TrainRecord tr;
        memset(&tr, 0, sizeof(tr));
        tr.train_id = 101;
        strncpy(tr.train_name, "Express-101", MAX_NAME_LEN - 1);
        tr.fare = 750.50;
        write(g_trains_fd, &tr, sizeof(tr));
    }
}

// Network Framed Packet Send/Receive
int send_packet(int fd, uint16_t opcode, uint16_t status, const void *payload, uint32_t len) {
    PacketHeader hdr;
    hdr.magic = htonl(PROTO_MAGIC);
    hdr.opcode = htons(opcode);
    hdr.status = htons(status);
    hdr.payload_len = htonl(len);

    if (write(fd, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (len > 0 && payload != NULL) {
        if (write(fd, payload, len) != (ssize_t)len) return -1;
    }
    return 0;
}

// Core Business Logic Handlers
void handle_login(int client_fd, const uint8_t *payload, uint32_t len) {
    if (len < sizeof(LoginReqPayload)) {
        send_packet(client_fd, OP_LOGIN_RESP, 400, NULL, 0);
        return;
    }

    LoginReqPayload *req = (LoginReqPayload *)payload;
    UserRecord u;
    off_t offset = 0;
    int found = 0;

    // Scan users with non-blocking read lock
    while (pread(g_users_fd, &u, sizeof(UserRecord), offset) == sizeof(UserRecord)) {
        if (strcmp(u.username, req->username) == 0 && strcmp(u.password, req->password) == 0) {
            if (u.role_id == req->role_id) {
                found = 1;
                break;
            }
        }
        offset += sizeof(UserRecord);
    }

    if (found) {
        LoginRespPayload resp;
        resp.user_id = u.user_id;
        resp.role_id = u.role_id;
        send_packet(client_fd, OP_LOGIN_RESP, 0, &resp, sizeof(resp));
    } else {
        send_packet(client_fd, OP_LOGIN_RESP, 401, NULL, 0); // Unauthorized
    }
}

void handle_view_train(int client_fd, const uint8_t *payload, uint32_t len) {
    if (len < sizeof(ViewTrainReqPayload)) {
        send_packet(client_fd, OP_VIEW_TRAIN_RESP, 400, NULL, 0);
        return;
    }

    ViewTrainReqPayload *req = (ViewTrainReqPayload *)payload;
    TrainRecord tr;
    off_t offset = 0;
    int found = 0;

    while (pread(g_trains_fd, &tr, sizeof(TrainRecord), offset) == sizeof(TrainRecord)) {
        if (tr.train_id == req->train_id) {
            found = 1;
            break;
        }
        offset += sizeof(TrainRecord);
    }

    if (!found) {
        send_packet(client_fd, OP_VIEW_TRAIN_RESP, 404, NULL, 0);
        return;
    }

    // Shared read-lock on this specific train record
    set_record_lock(g_trains_fd, F_RDLCK, offset, sizeof(TrainRecord), 1);

    ViewTrainRespPayload resp;
    resp.train_id = tr.train_id;
    strncpy(resp.train_name, tr.train_name, MAX_NAME_LEN);
    resp.base_fare = tr.fare;
    for (int i = 0; i < TOTAL_SEATS; i++) {
        resp.seat_map[i] = (tr.seats[i] != 0) ? 1 : 0;
    }

    set_record_lock(g_trains_fd, F_UNLCK, offset, sizeof(TrainRecord), 1);
    send_packet(client_fd, OP_VIEW_TRAIN_RESP, 0, &resp, sizeof(resp));
}

void handle_book_seat(int client_fd, const uint8_t *payload, uint32_t len) {
    if (len < sizeof(BookSeatReqPayload)) {
        send_packet(client_fd, OP_BOOK_SEAT_RESP, 400, NULL, 0);
        return;
    }

    BookSeatReqPayload *req = (BookSeatReqPayload *)payload;
    if (req->seat_number < 1 || req->seat_number > TOTAL_SEATS) {
        send_packet(client_fd, OP_BOOK_SEAT_RESP, 422, NULL, 0);
        return;
    }

    TrainRecord tr;
    off_t tr_offset = 0;
    int found = 0;

    while (pread(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset) == sizeof(TrainRecord)) {
        if (tr.train_id == req->train_id) {
            found = 1;
            break;
        }
        tr_offset += sizeof(TrainRecord);
    }

    if (!found) {
        send_packet(client_fd, OP_BOOK_SEAT_RESP, 404, NULL, 0);
        return;
    }

    // Exclusive write lock on the specific train record
    if (set_record_lock(g_trains_fd, F_WRLCK, tr_offset, sizeof(TrainRecord), 1) != 0) {
        send_packet(client_fd, OP_BOOK_SEAT_RESP, 503, NULL, 0);
        return;
    }

    // Re-read latest record state inside the locked critical section
    pread(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset);

    int seat_idx = req->seat_number - 1;
    if (tr.seats[seat_idx] != 0) {
        // Seat already taken
        set_record_lock(g_trains_fd, F_UNLCK, tr_offset, sizeof(TrainRecord), 1);
        send_packet(client_fd, OP_BOOK_SEAT_RESP, 409, NULL, 0); // Conflict
        return;
    }

    // Commit seat reservation
    tr.seats[seat_idx] = req->user_id;
    pwrite(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset);
    set_record_lock(g_trains_fd, F_UNLCK, tr_offset, sizeof(TrainRecord), 1);

    // Insert new booking record (atomic append using lock at file end)
    off_t bk_offset = lseek(g_bookings_fd, 0, SEEK_END);
    set_record_lock(g_bookings_fd, F_WRLCK, bk_offset, sizeof(BookingRecord), 1);

    int booking_id = (int)(bk_offset / sizeof(BookingRecord)) + 1;
    BookingRecord bk = {
        booking_id,
        req->user_id,
        req->train_id,
        req->seat_number,
        1,
        tr.fare
    };

    pwrite(g_bookings_fd, &bk, sizeof(BookingRecord), bk_offset);
    set_record_lock(g_bookings_fd, F_UNLCK, bk_offset, sizeof(BookingRecord), 1);

    BookSeatRespPayload resp;
    resp.booking_id = booking_id;
    resp.seat_number = req->seat_number;
    resp.fare_charged = tr.fare;

    send_packet(client_fd, OP_BOOK_SEAT_RESP, 0, &resp, sizeof(resp));
}

void handle_cancel_booking(int client_fd, const uint8_t *payload, uint32_t len) {
    if (len < sizeof(CancelBookingReqPayload)) {
        send_packet(client_fd, OP_CANCEL_BOOKING_RESP, 400, NULL, 0);
        return;
    }

    CancelBookingReqPayload *req = (CancelBookingReqPayload *)payload;
    off_t bk_offset = (off_t)(req->booking_id - 1) * sizeof(BookingRecord);

    if (set_record_lock(g_bookings_fd, F_WRLCK, bk_offset, sizeof(BookingRecord), 1) != 0) {
        send_packet(client_fd, OP_CANCEL_BOOKING_RESP, 503, NULL, 0);
        return;
    }

    BookingRecord bk;
    if (pread(g_bookings_fd, &bk, sizeof(BookingRecord), bk_offset) != sizeof(BookingRecord) ||
        bk.user_id != req->user_id || !bk.is_active) {
        set_record_lock(g_bookings_fd, F_UNLCK, bk_offset, sizeof(BookingRecord), 1);
        send_packet(client_fd, OP_CANCEL_BOOKING_RESP, 404, NULL, 0);
        return;
    }

    // Cancel booking record
    bk.is_active = 0;
    pwrite(g_bookings_fd, &bk, sizeof(BookingRecord), bk_offset);
    set_record_lock(g_bookings_fd, F_UNLCK, bk_offset, sizeof(BookingRecord), 1);

    // Release seat in train record
    TrainRecord tr;
    off_t tr_offset = 0;
    while (pread(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset) == sizeof(TrainRecord)) {
        if (tr.train_id == bk.train_id) {
            set_record_lock(g_trains_fd, F_WRLCK, tr_offset, sizeof(TrainRecord), 1);
            pread(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset);
            if (tr.seats[bk.seat_number - 1] == req->user_id) {
                tr.seats[bk.seat_number - 1] = 0;
                pwrite(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset);
            }
            set_record_lock(g_trains_fd, F_UNLCK, tr_offset, sizeof(TrainRecord), 1);
            break;
        }
        tr_offset += sizeof(TrainRecord);
    }

    send_packet(client_fd, OP_CANCEL_BOOKING_RESP, 0, NULL, 0);
}

void handle_update_fare(int client_fd, const uint8_t *payload, uint32_t len) {
    if (len < sizeof(UpdateFareReqPayload)) {
        send_packet(client_fd, OP_ADMIN_UPDATE_PRICE_RESP, 400, NULL, 0);
        return;
    }

    UpdateFareReqPayload *req = (UpdateFareReqPayload *)payload;
    TrainRecord tr;
    off_t tr_offset = 0;
    int found = 0;

    while (pread(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset) == sizeof(TrainRecord)) {
        if (tr.train_id == req->train_id) {
            found = 1;
            break;
        }
        tr_offset += sizeof(TrainRecord);
    }

    if (!found) {
        send_packet(client_fd, OP_ADMIN_UPDATE_PRICE_RESP, 404, NULL, 0);
        return;
    }

    set_record_lock(g_trains_fd, F_WRLCK, tr_offset, sizeof(TrainRecord), 1);
    pread(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset);
    tr.fare = req->new_fare;
    pwrite(g_trains_fd, &tr, sizeof(TrainRecord), tr_offset);
    set_record_lock(g_trains_fd, F_UNLCK, tr_offset, sizeof(TrainRecord), 1);

    send_packet(client_fd, OP_ADMIN_UPDATE_PRICE_RESP, 0, NULL, 0);
}

// Multiplexed Client Dispatcher
void process_client(int client_fd) {
    PacketHeader hdr;
    ssize_t n = read(client_fd, &hdr, sizeof(hdr));
    if (n <= 0) {
        close(client_fd);
        return;
    }

    if (ntohl(hdr.magic) != PROTO_MAGIC) {
        close(client_fd);
        return;
    }

    uint16_t opcode = ntohs(hdr.opcode);
    uint32_t payload_len = ntohl(hdr.payload_len);
    uint8_t payload[MAX_PAYLOAD];

    if (payload_len > MAX_PAYLOAD) {
        close(client_fd);
        return;
    }

    if (payload_len > 0) {
        size_t total = 0;
        while (total < payload_len) {
            ssize_t r = read(client_fd, payload + total, payload_len - total);
            if (r <= 0) { close(client_fd); return; }
            total += r;
        }
    }

    switch (opcode) {
        case OP_LOGIN_REQ:
            handle_login(client_fd, payload, payload_len);
            break;
        case OP_VIEW_TRAIN_REQ:
            handle_view_train(client_fd, payload, payload_len);
            break;
        case OP_BOOK_SEAT_REQ:
            handle_book_seat(client_fd, payload, payload_len);
            break;
        case OP_CANCEL_BOOKING_REQ:
            handle_cancel_booking(client_fd, payload, payload_len);
            break;
        case OP_ADMIN_UPDATE_PRICE_REQ:
            handle_update_fare(client_fd, payload, payload_len);
            break;
        default:
            send_packet(client_fd, opcode, 501, NULL, 0); // Not implemented
            break;
    }
}

int main() {
    init_databases();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Non-blocking socket for event loop
    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(SERVER_PORT);

    if (bind(listen_fd, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        perror("Bind failed");
        exit(1);
    }
    listen(listen_fd, 128);

    struct pollfd fds[MAX_CLIENTS];
    memset(fds, 0, sizeof(fds));
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;

    for (int i = 1; i < MAX_CLIENTS; i++) fds[i].fd = -1;

    printf("[Engine] Reservation Server listening on port %d with poll() loop...\n", SERVER_PORT);

    while (1) {
        int poll_count = poll(fds, MAX_CLIENTS, -1);
        if (poll_count < 0) {
            if (errno == EINTR) continue;
            perror("Poll error");
            break;
        }

        // Accept new connections
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int new_sock = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (new_sock >= 0) {
                fcntl(new_sock, F_SETFL, fcntl(new_sock, F_GETFL, 0) | O_NONBLOCK);
                int placed = 0;
                for (int i = 1; i < MAX_CLIENTS; i++) {
                    if (fds[i].fd == -1) {
                        fds[i].fd = new_sock;
                        fds[i].events = POLLIN;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    fprintf(stderr, "Max clients reached, dropping.\n");
                    close(new_sock);
                }
            }
        }

        // Process active clients
        for (int i = 1; i < MAX_CLIENTS; i++) {
            if (fds[i].fd > 0 && (fds[i].revents & POLLIN)) {
                process_client(fds[i].fd);
            }
            if (fds[i].fd > 0 && (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
    }

    close(listen_fd);
    close(g_users_fd);
    close(g_trains_fd);
    close(g_bookings_fd);
    return 0;
}
