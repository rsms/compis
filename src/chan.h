#pragma once
ASSUME_NONNULL_BEGIN

// chan_t is a messaging channel for CSP-like processing, with optional buffering.
//
//   usize buffer_cap = 4;
//   chan_t* c = chan_open(ma, sizeof(int), buffer_cap);
//
//   int send_messages[] = { 123, 456 };
//   chan_send(c, &send_messages[0]);
//   chan_send(c, &send_messages[1]);
//
//   int recv_messages[] = { 0, 0 };
//   chan_recv(c, &recv_messages[0]);
//   chan_recv(c, &recv_messages[1]);
//
//   assert(recv_messages[0] == send_messages[0]);
//   assert(recv_messages[1] == send_messages[1]);
//
//   chan_close(c);
//   chan_free(c);
//
typedef struct chan_t chan_t; // opaque

// chan_open creates and initializes a new channel which holds elements
// of elemsize byte. If bufcap>0 then a buffered channel with the capacity
// to hold bufcap elements is created.
chan_t* nullable chan_open(memalloc_t ma, usize elemsize, u32 bufcap);

// chan_close cancels any waiting senders and receivers.
// Messages sent before this call are guaranteed to be delivered, assuming there are
// active receivers. Once a channel is closed it can not be reopened nor sent to.
// chan_close must only be called once per channel.
void chan_close(chan_t*);

// chan_free frees memory of a channel
void chan_free(chan_t*);

// chan_cap returns the channel's buffer capacity
u32 chan_cap(const chan_t* c);

// chan_send enqueues a message to a channel by copying the value at elemptr
// to the channel. Blocks until the message is sent or the channel is closed.
// Returns false if the channel closed.
bool chan_send(chan_t*, void* elemptr);

// chan_recv dequeues a message from a channel by copying a received value to elemptr.
// Blocks until there's a message available or the channel is closed.
// Returns true if a message was received, false if the channel is closed.
bool chan_recv(chan_t*, void* elemptr);

// chan_trysend attempts to sends a message without blocking.
// It returns true if the message was sent, false if not.
// Unlike chan_send, this function does not return false to indicate that the channel
// is closed, but instead it returns false if the message was not sent and sets *closed
// to false if the reason for the failure was a closed channel.
bool chan_trysend(chan_t*, void* elemptr, bool* closed);

// chan_tryrecv works like chan_recv but does not block.
// Returns true if a message was received.
// This function does not block/wait.
bool chan_tryrecv(chan_t* ch, void* elemptr, bool* closed);

ASSUME_NONNULL_END
