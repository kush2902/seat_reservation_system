#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>

#define SERVER_PORT 9090
#define PROTO_MAGIC 0x53595343 // "SYSC"
#define MAX_PAYLOAD 4096
#define MAX_NAME_LEN 32
#define TOTAL_SEATS 60

// Role definitions
#define ROLE_PASSENGER 1
#define ROLE_ADMIN     2

// Opcodes
enum {
    OP_LOGIN_REQ = 1,
    OP_LOGIN_RESP,
    OP_VIEW_TRAIN_REQ,
    OP_VIEW_TRAIN_RESP,
    OP_BOOK_SEAT_REQ,
    OP_BOOK_SEAT_RESP,
    OP_CANCEL_BOOKING_REQ,
    OP_CANCEL_BOOKING_RESP,
    OP_VIEW_MY_BOOKINGS_REQ,
    OP_VIEW_MY_BOOKINGS_RESP,
    OP_ADMIN_UPDATE_PRICE_REQ,
    OP_ADMIN_UPDATE_PRICE_RESP
};

// Protocol Header (Packed: 12 bytes total)
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;       // PROTO_MAGIC
    uint16_t opcode;      // Operation identifier
    uint16_t status;      // 0 = Success, >0 = Error Code
    uint32_t payload_len; // Length of following body
} PacketHeader;

// Payload Structures
typedef struct {
    int32_t  role_id;
    char     username[MAX_NAME_LEN];
    char     password[MAX_NAME_LEN];
} LoginReqPayload;

typedef struct {
    int32_t  user_id;
    int32_t  role_id;
} LoginRespPayload;

typedef struct {
    int32_t  train_id;
} ViewTrainReqPayload;

typedef struct {
    int32_t  train_id;
    char     train_name[MAX_NAME_LEN];
    double   base_fare;
    uint8_t  seat_map[TOTAL_SEATS]; // 0 = Available, 1 = Booked
} ViewTrainRespPayload;

typedef struct {
    int32_t  user_id;
    int32_t  train_id;
    int32_t  seat_number; // 1-based index (1 to TOTAL_SEATS)
} BookSeatReqPayload;

typedef struct {
    int32_t  booking_id;
    int32_t  seat_number;
    double   fare_charged;
} BookSeatRespPayload;

typedef struct {
    int32_t  user_id;
    int32_t  booking_id;
} CancelBookingReqPayload;

typedef struct {
    int32_t  user_id;
    int32_t  train_id;
    double   new_fare;
} UpdateFareReqPayload;
#pragma pack(pop)

// Binary Disk Records
typedef struct {
    int32_t  user_id;
    int32_t  role_id;
    char     username[MAX_NAME_LEN];
    char     password[MAX_NAME_LEN];
} UserRecord;

typedef struct {
    int32_t  train_id;
    char     train_name[MAX_NAME_LEN];
    double   fare;
    int32_t  seats[TOTAL_SEATS]; // 0 = empty, otherwise contains user_id
} TrainRecord;

typedef struct {
    int32_t  booking_id;
    int32_t  user_id;
    int32_t  train_id;
    int32_t  seat_number;
    int32_t  is_active; // 1 = active, 0 = cancelled
    double   fare;
} BookingRecord;

#endif
