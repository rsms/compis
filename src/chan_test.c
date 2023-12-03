#include "colib.h"

#ifdef CO_ENABLE_TESTS

#include "chan.h"
#include "thread.h"

ASSUME_NONNULL_BEGIN

// silence dlog
#if 1
  #undef dlog
  #define dlog(...) ((void)0)
#endif

// tests rely on assertions
#ifndef DEBUG
  #undef assert
  #undef assertf
  #undef assertcstreq
  #undef assertnull
  #undef assertnotnull
  #undef assert_no_add_overflow
  #undef assert_no_sub_overflow
  #undef assert_no_mul_overflow

  #define assert(cond)                 safecheckx(cond)
  #define assertf(cond, fmt, args...)  safecheckxf((cond), (fmt), ##args)
#endif

#define asserteq(a,b) assert((a) == (b))


typedef u32 Msg;


static u64 init_test_messages(Msg* messages, u32 nmessages) {
  // init messages (1 2 3 ...)
  u64 messages_sum = 0; // sum of all messages
  for (u32 i = 0; i < nmessages; i++) {
    Msg msg = (Msg)i + 1; // make it 1-based for simplicity
    messages[i] = msg;
    messages_sum += (u64)msg;
  }
  return messages_sum;
}


UNITTEST_DEF(chan_0_test) {
  memalloc_t ma = memalloc_ctx();
  Msg messages[10]; // must be an even number
  u64 send_messages_sum = init_test_messages(messages, countof(messages));
  u64 recv_messages_sum = 0; // sum of all messages received

  size_t N = 2;
  chan_t* ch = chan_open(ma, sizeof(Msg), /*bufsize*/N);

  for (size_t i = 0; i < countof(messages); i += N) {
    for (size_t j = 0; j < N; j++) {
      chan_send(ch, &messages[i + j]);
    }
    for (size_t j = 0; j < N; j++) {
      Msg msg_out;
      assert(chan_recv(ch, &msg_out));
      asserteq(messages[i + j], msg_out);
      recv_messages_sum += (u64)msg_out;
    }
  }

  asserteq(send_messages_sum, recv_messages_sum);

  chan_close(ch);
  chan_free(ch);
}


// TODO: test non-blocking chan_trysend and chan_tryrecv


typedef struct TestThread {
  thrd_t  t;
  u32     id;
  chan_t* ch;

  // messages received or to be sent
  u32  msgcap; // capacity of msgv
  u32  msglen; // messages in msgv
  Msg* msgv;   // messages
} TestThread;


static int send_thread(void* tptr) {
  TestThread* t = (TestThread*)tptr;
  for (u32 i = 0; i < t->msglen; i++) {
    Msg msg = t->msgv[i];
    bool ok = chan_send(t->ch, &msg);
    assertf(ok, "[send_thread#%u] channel closed during send", t->id);
  }
  //dlog("[send_thread#%u] exit", t->id);
  return 0;
}


static int recv_thread(void* tptr) {
  TestThread* t = (TestThread*)tptr;
  t->msglen = 0;
  while (1) {
    // receive a message
    Msg msg;
    if (!chan_recv(t->ch, &msg)) {
      break; // channel closed
    }

    // save received message for later inspection by test
    assertf(t->msglen < t->msgcap,
      "[recv_thread#%u] received an excessive number of messages (>%u)",
      t->id, t->msgcap);
    t->msgv[t->msglen++] = msg;

    // add some thread scheduling jitter
    // usleep(rand() % 10);
  }
  //dlog("[recv_thread#%u] exit", t->id);
  return 0;
}


static void chan_1send_Nrecv(u32 bufcap, u32 n_send_threads, u32 n_recv_threads, u32 nmessages);

#define T UNITTEST_DEF
T(chan_1send_1recv_buffered)  { chan_1send_Nrecv(2,            1,            1, 40); }
T(chan_1send_Nrecv_buffered)  { chan_1send_Nrecv(2,            1, sys_ncpu()+1, 40); }
T(chan_Nsend_1recv_buffered)  { chan_1send_Nrecv(2, sys_ncpu()+1,            1, 40); }
T(chan_Nsend_Nrecv_buffered)  { chan_1send_Nrecv(2, sys_ncpu()+1, sys_ncpu()+1, 40); }
T(chan_1send_1recv_unbuffered){ chan_1send_Nrecv(0,            1,            1, 40); }
T(chan_1send_Nrecv_unbuffered){ chan_1send_Nrecv(0,            1, sys_ncpu()+1, 40); }
T(chan_Nsend_1recv_unbuffered){ chan_1send_Nrecv(0, sys_ncpu()+1,            1, 40); }
T(chan_Nsend_Nrecv_unbuffered){ chan_1send_Nrecv(0, sys_ncpu()+1, sys_ncpu()+1, 40); }
T(chan_1send_Nrecv_buffered1) { chan_1send_Nrecv(1, 2, 2, 8); }
#undef T


static void chan_1send_Nrecv(
  u32 bufcap, u32 n_send_threads, u32 n_recv_threads, u32 nmessages)
{
  // serial sender, multiple receivers
  memalloc_t ma = memalloc_ctx();

  u32 send_message_count = MAX(n_recv_threads, n_send_threads) * nmessages;
  TestThread* recv_threads = mem_alloctv(ma, TestThread, n_recv_threads);
  TestThread* send_threads = mem_alloctv(ma, TestThread, n_send_threads);

  // allocate storage for messages
  // the calling "sender" thread uses send_message_count messages while each
  // receiver thread is given send_message_count message slots for reception
  // as in theory one thread may receive all messages.
  size_t message_storage_count = send_message_count * (n_recv_threads + 1);
  Msg* message_storage = mem_alloctv(ma, Msg, message_storage_count);
  Msg* send_messages = &message_storage[0];

  chan_t* ch = chan_open(ma, sizeof(Msg), /*cap*/bufcap);
  dlog("channel capacity: %zu, send_message_count: %u",
    chan_cap(ch), send_message_count);

  // init messages (1 2 3 ...)
  u64 send_message_sum = 0; // sum of all messages
  for (u32 i = 0; i < send_message_count; i++) {
    Msg msg = (Msg)i + 1; // make it 1-based for simplicity
    send_messages[i] = msg;
    send_message_sum += (u64)msg;
  }

  dlog("spawning %u sender threads", n_send_threads);
  u32 send_messages_i = 0;
  const u32 send_messages_n = send_message_count / n_send_threads;
  for (u32 i = 0; i < n_send_threads; i++) {
    TestThread* t = &send_threads[i];
    t->id = i + 1;
    t->ch = ch;
    t->msgcap = send_message_count;

    assert(send_messages_i < send_message_count);
    t->msgv = &send_messages[send_messages_i];

    u32 end_i = (
      i < n_send_threads-1 ? MIN(send_messages_i + send_messages_n, send_message_count) :
      send_message_count // last chunk
    );
    // dlog("send_thread %u sends messages [%02u-%02u)", t->id, send_messages_i, end_i);
    t->msglen = end_i - send_messages_i;
    send_messages_i = end_i;

    int status = thrd_create(&t->t, send_thread, t);
    asserteq(status, thrd_success);
  }

  dlog("spawning %u receiver threads", n_recv_threads);
  for (u32 i = 0; i < n_recv_threads; i++) {
    TestThread* t = &recv_threads[i];
    t->id = i + 1;
    t->ch = ch;
    t->msgcap = send_message_count;
    t->msgv = &message_storage[(i + 1) * send_message_count];
    int status = thrd_create(&t->t, recv_thread, t);
    asserteq(status, thrd_success);
  }

  // wait for all messages to be sent
  dlog("waiting for %u messages to be sent by %u threads...", send_message_count, n_send_threads);
  for (u32 i = 0; i < n_send_threads; i++) {
    int retval;
    thrd_join(send_threads[i].t, &retval);
  }
  dlog("done sending %u messages", send_message_count);
  //msleep(100);

  // close the channel and wait for receiver threads to exit
  chan_close(ch);
  dlog("waiting for %u receiver threads to finish...", n_recv_threads);
  for (u32 i = 0; i < n_recv_threads; i++) {
    int retval;
    thrd_join(recv_threads[i].t, &retval);
  }
  chan_free(ch);

  // check results
  u32 recv_message_count = 0; // tally of total number of messages all threads received
  u64 recv_message_sum   = 0; // sum of all messages received
  for (u32 i = 0; i < n_recv_threads; i++) {
    TestThread* t = &recv_threads[i];
    recv_message_count += t->msglen;
    for (u32 y = 0; y < t->msglen; y++) {
      recv_message_sum += (u64)t->msgv[y];
    }
  }
  asserteq(recv_message_count, send_message_count);
  asserteq(recv_message_sum, send_message_sum);

  mem_freetv(ma, recv_threads, n_recv_threads);
  mem_freetv(ma, send_threads, n_send_threads);
  mem_freetv(ma, send_messages, message_storage_count);
}


ASSUME_NONNULL_END
#endif // defined(CO_ENABLE_TESTS)
