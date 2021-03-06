// Copyright 2019 Josh Pieper, jjp@pobox.com.
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

#include "mjlib/multiplex/frame_stream.h"

#include <boost/asio/write.hpp>
#include <boost/test/auto_unit_test.hpp>

#include "mjlib/io/stream_pipe_factory.h"
#include "mjlib/io/test/reader.h"

namespace io = mjlib::io;
using mjlib::multiplex::Frame;
using mjlib::multiplex::FrameStream;

namespace {
struct Fixture {
  void Poll() {
    service.poll();
    service.reset();
  }

  boost::asio::io_service service;
  io::StreamPipeFactory pipe_factory{service};
  io::SharedStream client_side{pipe_factory.GetStream("", 1)};
  FrameStream dut{client_side.get()};

  io::SharedStream server_side{pipe_factory.GetStream("", 0)};
  io::test::Reader server_reader{server_side.get()};
};
}

BOOST_FIXTURE_TEST_CASE(FrameStreamWriteTest, Fixture) {
  Frame to_send;
  to_send.source_id = 1;
  to_send.dest_id = 2;
  to_send.request_reply = false;

  int write_done = 0;
  dut.AsyncWrite(&to_send, [&](const mjlib::base::error_code& ec) {
      mjlib::base::FailIf(ec);
      write_done++;
    });

  BOOST_TEST(write_done == 0);
  BOOST_TEST(server_reader.data().empty());

  Poll();

  BOOST_TEST(write_done == 1);
  BOOST_TEST(server_reader.data() ==
             std::string("\x54\xab\x01\x02\x00\x03\x28", 7));
}

BOOST_FIXTURE_TEST_CASE(FrameStreamReadTest, Fixture) {
  Frame to_receive;
  int read_done = 0;

  dut.AsyncRead(&to_receive, {}, [&](auto&& ec) {
      mjlib::base::FailIf(ec);
      read_done++;
    });

  BOOST_TEST(read_done == 0);
  Poll();
  BOOST_TEST(read_done == 0);

  int write_done = 0;
  // Now write a frame into the server side.
  boost::asio::async_write(
      *server_side,
      boost::asio::buffer("\x54\xab\x04\x05\x01\x20\xec\x88", 8),
      [&](auto&& ec, size_t size) {
        mjlib::base::FailIf(ec);
        write_done++;
        BOOST_TEST(size == 8);
      });

  BOOST_TEST(write_done == 0);

  Poll();
  BOOST_TEST(read_done == 1);
  BOOST_TEST(write_done == 1);

  BOOST_TEST(to_receive.source_id == 4);
  BOOST_TEST(to_receive.dest_id == 5);
  BOOST_TEST(to_receive.payload == " ");
}
