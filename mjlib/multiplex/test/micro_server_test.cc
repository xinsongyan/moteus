// Copyright 2018-2019 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mjlib/multiplex/micro_server.h"

#include <boost/test/auto_unit_test.hpp>

#include "mjlib/micro/stream_pipe.h"
#include "mjlib/micro/test/persistent_config_fixture.h"
#include "mjlib/micro/test/str.h"

namespace base = mjlib::base;
using namespace mjlib::multiplex;
using mjlib::micro::test::str;
namespace micro = mjlib::micro;
namespace test = micro::test;

namespace {
class Server : public MicroServer::Server {
 public:
  uint32_t Write(MicroServer::Register reg,
                 const MicroServer::Value& value) override {
    writes_.push_back({reg, value});
    return next_write_error_;
  }

  MicroServer::ReadResult Read(
      MicroServer::Register reg, size_t type_index) const override {
    if (type_index == 2) {
      return MicroServer::Value(int32_values.at(reg));
    } else if (type_index == 3) {
      return MicroServer::Value(float_values.at(reg));
    }
    return static_cast<uint32_t>(1);
  }

  struct WriteValue {
    MicroServer::Register reg;
    MicroServer::Value value;
  };

  std::vector<WriteValue> writes_;
  uint32_t next_write_error_ = 0;

  std::map<uint32_t, int32_t> int32_values = {
    { 9, 0x09080706, },
  };
  std::map<uint32_t, float> float_values = {
    { 10, 1.0f },
    { 11, 2.0f },
  };
};

struct Fixture : test::PersistentConfigFixture {
  micro::StreamPipe dut_stream{event_queue.MakePoster()};

  Server server;
  MicroServer dut{&pool, dut_stream.side_b(), []() {
      return MicroServer::Options();
    }()};

  micro::AsyncStream* tunnel{dut.MakeTunnel(9)};

  Fixture() {
    dut.Start(&server);
  }
};

// Now send in a valid frame that contains some data.
const uint8_t kClientToServer[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x01,  // destination id
  0x0b,  // payload size
    0x40,  // client->server data
      0x09,  // channel 9
      0x08,  // data len
      't', 'e', 's', 't', ' ', 'a', 'n', 'd',
  0x62, 0x0f,  // CRC
  0x00,  // null terminator
};

const uint8_t kClientToServer2[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x02,  // destination id
  0x0b,  // payload size
    0x40,  // client->server data
      0x09,  // channel 9
      0x08,  // data len
      't', 'e', 's', 't', ' ', 'a', 'n', 'd',
  0xc7, 0xc0,  // CRC
  0x00,  // null terminator
};

const uint8_t kClientToServerMultiple[] = {
  0x54, 0xab,  // header
  0x02,  // source id
  0x01,  // destination id
  0x0b,  // payload size
    0x40,  // client->server data
      0x09,  // channel 9
      0x08,  // data len
      'f', 'i', 'r', 's', 't', ' ', 'f', 'm',
  0xc6, 0x17,  // CRC

  0x54, 0xab,  // header
  0x02,  // source id
  0x01,  // destination id
  0x09,  // payload size
    0x40,  // client->server data
      0x09,  // channel 9
      0x06,  // data len
      'm', 'o', 'r', 'e', 's', 't',
  0x09, 0x68,  // CRC
  0x00,  // null terminator
};
}

BOOST_FIXTURE_TEST_CASE(MicroServerTest, Fixture) {
  char read_buffer[100] = {};
  int read_count = 0;
  ssize_t read_size = 0;
  tunnel->AsyncReadSome(read_buffer, [&](micro::error_code ec, ssize_t size) {
      BOOST_TEST(!ec);
      read_count++;
      read_size = size;
    });

  BOOST_TEST(read_count == 0);
  event_queue.Poll();
  BOOST_TEST(read_count == 0);

  {
    int write_count = 0;
    AsyncWrite(*dut_stream.side_a(), str(kClientToServer),
               [&](micro::error_code ec) {
                 BOOST_TEST(!ec);
                 write_count++;
               });
    event_queue.Poll();
    BOOST_TEST(write_count == 1);
    BOOST_TEST(read_count == 1);
    BOOST_TEST(read_size == 8);
    BOOST_TEST(std::string_view(read_buffer, 8) == "test and");
  }
}

BOOST_FIXTURE_TEST_CASE(ServerWrongId, Fixture) {
  char read_buffer[100] = {};
  int read_count = 0;
  tunnel->AsyncReadSome(read_buffer, [&](micro::error_code, ssize_t) {
      read_count++;
    });

  char unknown_buf[100] = {};
  int unknown_count = 0;
  ssize_t unknown_size = 0;
  dut.AsyncReadUnknown(unknown_buf, [&](micro::error_code ec, ssize_t size) {
      BOOST_TEST(!ec);
      unknown_count++;
      unknown_size = size;
    });

  event_queue.Poll();
  BOOST_TEST(read_count == 0);
  BOOST_TEST(unknown_size == 0);

  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kClientToServer2),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });
  event_queue.Poll();
  BOOST_TEST(write_count == 1);
  BOOST_TEST(read_count == 0);
  BOOST_TEST(dut.stats()->wrong_id == 1);

  BOOST_TEST(unknown_count == 1);
  BOOST_TEST(unknown_size == str(kClientToServer2).size());
  BOOST_TEST(std::string(unknown_buf, unknown_size) ==
             str(kClientToServer2));
}

BOOST_FIXTURE_TEST_CASE(ServerTestReadSecond, Fixture) {
  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kClientToServer),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });
  event_queue.Poll();
  BOOST_TEST(write_count == 1);

  char read_buffer[100] = {};
  int read_count = 0;
  ssize_t read_size = 0;
  tunnel->AsyncReadSome(read_buffer, [&](micro::error_code ec, ssize_t size) {
      BOOST_TEST(!ec);
      read_count++;
      read_size = size;
    });

  event_queue.Poll();
  BOOST_TEST(read_count == 1);
  BOOST_TEST(read_size == 8);
  BOOST_TEST(std::string_view(read_buffer, 8) == "test and");
}

BOOST_FIXTURE_TEST_CASE(ServerTestFragment, Fixture) {
  AsyncWrite(*dut_stream.side_a(), str(kClientToServerMultiple),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
             });

  char read_buffer[3] = {};
  auto read = [&](const std::string& expected) {
    int read_count = 0;
    ssize_t read_size = 0;
    tunnel->AsyncReadSome(read_buffer, [&](micro::error_code ec, ssize_t size) {
        BOOST_TEST(!ec);
        read_count++;
        read_size = size;
      });
    event_queue.Poll();
    BOOST_TEST(read_count == 1);
    BOOST_TEST(read_size == expected.size());
    BOOST_TEST(std::string_view(read_buffer, read_size) == expected);
  };
  read("fir");
  read("st ");
  read("fmm");
  read("ore");
  read("st");

  // Now kick off a read that should stall until more data comes in.
  int read_count = 0;
  ssize_t read_size = 0;
  tunnel->AsyncReadSome(read_buffer, [&](micro::error_code ec, ssize_t size) {
      BOOST_TEST(!ec);
      read_count++;
      read_size = size;
    });

  event_queue.Poll();
  BOOST_TEST(read_count == 0);

  AsyncWrite(*dut_stream.side_a(), str(kClientToServer),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
             });
  event_queue.Poll();
  BOOST_TEST(read_count == 1);
  BOOST_TEST(read_size == 3);
  BOOST_TEST(std::string_view(read_buffer, 3) == "tes");
}

namespace {
const uint8_t kClientToServerEmpty[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x01,  // destination id
  0x03,  // payload size
    0x40,  // client->server data
      0x09,  // channel 9
      0x00,  // data len
  0xcf, 0xb2,  // CRC
  0x00,  // null terminator
};
}

BOOST_FIXTURE_TEST_CASE(ServerSendTest, Fixture) {
  int write_count = 0;
  ssize_t write_size = 0;
  tunnel->AsyncWriteSome(
      "stuff to test",
      [&](micro::error_code ec, ssize_t size) {
        BOOST_TEST(!ec);
        write_count++;
        write_size = size;
      });

  event_queue.Poll();
  BOOST_TEST(write_count == 0);

  char receive_buffer[256] = {};
  int read_count = 0;
  ssize_t read_size = 0;
  dut_stream.side_a()->AsyncReadSome(
      receive_buffer, [&](micro::error_code ec, ssize_t size) {
        BOOST_TEST(!ec);
        read_count++;
        read_size = size;
      });

  event_queue.Poll();
  BOOST_TEST(write_count == 0);
  BOOST_TEST(read_count == 0);

  AsyncWrite(*dut_stream.side_a(), str(kClientToServerEmpty),
             [](micro::error_code ec) { BOOST_TEST(!ec); });

  event_queue.Poll();

  BOOST_TEST(write_count == 1);
  BOOST_TEST(read_count == 1);

  const uint8_t kExpectedResponse[] = {
    0x54, 0xab,
    0x01,  // source id
    0x02,  // dest id
    0x10,  // payload size
     0x41,  // server->client
      0x09,  // channel 9
      0x0d,  // 13 bytes of data
      's', 't', 'u', 'f', 'f', ' ', 't', 'o', ' ', 't', 'e', 's', 't',
    0x9d, 0xd2,  // CRC
    0x00,  // null terminator
  };

  BOOST_TEST(std::string_view(receive_buffer, read_size) ==
             str(kExpectedResponse));
}

namespace {
const uint8_t kWriteSingle[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x01,  // destination id
  0x03,  // payload size
    0x10,  // write single int8_t
      0x02,  // register 2
      0x20,  // value
  0x99, 0x14,  // CRC
  0x00,  // null terminator
};
}

BOOST_FIXTURE_TEST_CASE(WriteSingleTest, Fixture) {
  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kWriteSingle),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });

  event_queue.Poll();
  BOOST_TEST(write_count == 1);

  BOOST_TEST(server.writes_.size() == 1);
  BOOST_TEST(server.writes_.at(0).reg == 2);
  BOOST_TEST(std::get<int8_t>(server.writes_.at(0).value) == 0x20);
}

namespace {
const uint8_t kWriteMultiple[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x01,  // destination id
  0x09,  // payload size
    0x15,  // write multiple int16_t
      0x05,  // start register
      0x03,  // 3x registers
      0x01, 0x03,  // value1
      0x03, 0x03,  // value1
      0x05, 0x03,  // value1
  0xda, 0xa6,  // CRC
  0x00,  // null terminator
};
}

BOOST_FIXTURE_TEST_CASE(WriteMultipleTest, Fixture) {
  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kWriteMultiple),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });

  event_queue.Poll();
  BOOST_TEST(write_count == 1);

  BOOST_TEST(server.writes_.size() == 3);
  BOOST_TEST(server.writes_.at(0).reg == 5);
  BOOST_TEST(std::get<int16_t>(server.writes_.at(0).value) == 0x0301);

  BOOST_TEST(server.writes_.at(1).reg == 6);
  BOOST_TEST(std::get<int16_t>(server.writes_.at(1).value) == 0x0303);

  BOOST_TEST(server.writes_.at(2).reg == 7);
  BOOST_TEST(std::get<int16_t>(server.writes_.at(2).value) == 0x0305);
}

BOOST_FIXTURE_TEST_CASE(WriteErrorTest, Fixture) {
  char receive_buffer[256] = {};
  int read_count = 0;
  ssize_t read_size = 0;
  dut_stream.side_a()->AsyncReadSome(
      receive_buffer, [&](micro::error_code ec, ssize_t size) {
        BOOST_TEST(!ec);
        read_count++;
        read_size = size;
      });

  event_queue.Poll();
  BOOST_TEST(read_count == 0);


  server.next_write_error_ = 0x76;

  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kWriteSingle),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });

  event_queue.Poll();
  BOOST_TEST(write_count == 1);
  BOOST_TEST(read_count == 1);

  const uint8_t kExpectedResponse[] = {
    0x54, 0xab,
    0x01,  // source id
    0x02,  // dest id
    0x03,  // payload size
     0x28,  // write error
      0x02,  // register
      0x76,  // error
    0xbc, 0xb6,  // CRC
    0x00,  // null terminator
  };

  BOOST_TEST(std::string_view(receive_buffer, read_size) ==
             str(kExpectedResponse));
}

namespace {
const uint8_t kReadSingle[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x01,  // destination id
  0x02,  // payload size
    0x1a,  // read single int32_t
      0x09,  // register
  0x03, 0xfb,  // CRC
  0x00,  // null terminator
};
}

BOOST_FIXTURE_TEST_CASE(ReadSingleTest, Fixture) {
  char receive_buffer[256] = {};
  int read_count = 0;
  ssize_t read_size = 0;
  dut_stream.side_a()->AsyncReadSome(
      receive_buffer, [&](micro::error_code ec, ssize_t size) {
        BOOST_TEST(!ec);
        read_count++;
        read_size = size;
      });

  event_queue.Poll();

  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kReadSingle),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });

  event_queue.Poll();
  BOOST_TEST(write_count == 1);
  BOOST_TEST(read_count == 1);

  const uint8_t kExpectedResponse[] = {
    0x54, 0xab,
    0x01,  // source id
    0x02,  // dest id
    0x06,  // payload size
     0x22,  // reply single int32_t
      0x09,  // register
      0x06, 0x07, 0x08, 0x09,  // value
    0x00, 0xdb,  // CRC
    0x00,  // null terminator
  };

  BOOST_TEST(std::string_view(receive_buffer, read_size) ==
             str(kExpectedResponse));
}

namespace {
const uint8_t kReadMultiple[] = {
  0x54, 0xab,  // header
  0x82,  // source id
  0x01,  // destination id
  0x03,  // payload size
    0x1f,  // read multiple float
      0x0a,  // register
      0x02,  // two things
  0x21, 0xb5,  // CRC
  0x00,  // null terminator
};
}

BOOST_FIXTURE_TEST_CASE(ReadMultipleTest, Fixture) {
  char receive_buffer[256] = {};
  int read_count = 0;
  ssize_t read_size = 0;
  dut_stream.side_a()->AsyncReadSome(
      receive_buffer, [&](micro::error_code ec, ssize_t size) {
        BOOST_TEST(!ec);
        read_count++;
        read_size = size;
      });

  event_queue.Poll();

  int write_count = 0;
  AsyncWrite(*dut_stream.side_a(), str(kReadMultiple),
             [&](micro::error_code ec) {
               BOOST_TEST(!ec);
               write_count++;
             });

  event_queue.Poll();
  BOOST_TEST(write_count == 1);
  BOOST_TEST(read_count == 1);

  const uint8_t kExpectedResponse[] = {
    0x54, 0xab,
    0x01,  // source id
    0x02,  // dest id
    0x0c,  // payload size
     0x23,  // reply single float
      0x0a,  // register
      0x00, 0x00, 0x80, 0x3f,  // value
     0x23,  // reply single float
      0x0b,
      0x00, 0x00, 0x00, 0x40,
    0x31, 0x1b,  // CRC
    0x00,  // null terminator
  };

  BOOST_TEST(std::string_view(receive_buffer, read_size) ==
             str(kExpectedResponse));
}