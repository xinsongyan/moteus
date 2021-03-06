// Copyright 2015-2019 Josh Pieper, jjp@pobox.com.  All rights reserved.
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

#include "mjlib/io/stream_factory.h"

#include <functional>
#include <iostream>

#include <boost/asio/read.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/exception/all.hpp>
#include <boost/program_options.hpp>

#include "mjlib/base/fail.h"
#include "mjlib/base/program_options_archive.h"

namespace pl = std::placeholders;
namespace base = mjlib::base;
namespace io = mjlib::io;
namespace po = boost::program_options;

namespace {
class Communicator {
 public:
  Communicator(io::StreamFactory* stream_factory,
               const io::StreamFactory::Options& options) {
    io::StreamFactory::Options stdio_options;
    stdio_options.type = io::StreamFactory::Type::kStdio;

    stream_factory->AsyncCreate(
        stdio_options,
        std::bind(&Communicator::HandleStdio, this, pl::_1, pl::_2));
    stream_factory->AsyncCreate(
        options,
        std::bind(&Communicator::HandleRemote, this, pl::_1, pl::_2));
  }

  void HandleStdio(const base::error_code& ec, io::SharedStream stream) {
    base::FailIf(ec);

    stdio_ = stream;
    MaybeStart();
  }

  void HandleRemote(const base::error_code& ec, io::SharedStream stream) {
    base::FailIf(ec);

    remote_ = stream;
    MaybeStart();
  }

  void MaybeStart() {
    if (!stdio_) { return; }
    if (!remote_) { return; }

    StartReadStdio();
    StartReadRemote();
  }

  void StartReadStdio() {
    stdio_->async_read_some(
        boost::asio::buffer(stdio_buf_),
        std::bind(&Communicator::HandleReadStdio, this,
                  pl::_1, pl::_2));
  }

  void HandleReadStdio(const base::error_code& ec, size_t size) {
    base::FailIf(ec);
    boost::asio::async_write(*remote_, boost::asio::buffer(stdio_buf_, size),
                             std::bind(&Communicator::HandleWriteRemote, this,
                                       pl::_1, pl::_2));
  }

  void HandleWriteRemote(const base::error_code& ec, size_t) {
    base::FailIf(ec);
    StartReadStdio();
  }

  void StartReadRemote() {
    remote_->async_read_some(
        boost::asio::buffer(remote_buf_),
        std::bind(&Communicator::HandleReadRemote, this,
                  pl::_1, pl::_2));
  }

  void HandleReadRemote(const base::error_code& ec, size_t size) {
    base::FailIf(ec);
    boost::asio::async_write(*stdio_, boost::asio::buffer(remote_buf_, size),
                             std::bind(&Communicator::HandleWriteStdio, this,
                                       pl::_1, pl::_2));
  }

  void HandleWriteStdio(const base::error_code& ec, size_t) {
    base::FailIf(ec);
    StartReadRemote();
  }

  io::SharedStream stdio_;
  io::SharedStream remote_;
  char stdio_buf_[4096] = {};
  char remote_buf_[4096] = {};
};
}

int main(int argc, char** argv) {
  boost::asio::io_service service;
  io::StreamFactory factory(service);

  io::StreamFactory::Options options;
  po::options_description desc("Allowable options");

  desc.add_options()("help,h", "display usage message");
  base::ProgramOptionsArchive(&desc).Accept(&options);

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cerr << desc;
    return 1;
  }

  Communicator communicator{&factory, options};

  service.run();
  return 0;
}
