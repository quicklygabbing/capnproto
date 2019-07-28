// Copyright (c) 2016 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "readiness-io.h"
#include <kj/test.h>
#include <stdlib.h>

namespace kj {
namespace {

KJ_TEST("readiness IO: write small") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  auto pipe = kj::newOneWayPipe();

  char buf[4];
  auto readPromise = pipe.in->read(buf, 3, 4);

  ReadyOutputStreamWrapper out(*pipe.out);
  KJ_ASSERT(KJ_ASSERT_NONNULL(out.write(kj::StringPtr("foo").asBytes())) == 3);

  // Without a flush, the data is still in the buffer.
  KJ_EXPECT(!readPromise.poll(ws));

  // So try flushing.
  out.flush().wait(ws);

  // Now our read has completed.
  KJ_ASSERT(readPromise.wait(ws) == 3);
  buf[3] = '\0';
  KJ_ASSERT(kj::StringPtr(buf) == "foo");
}

KJ_TEST("readiness IO: write many odd") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  auto pipe = kj::newOneWayPipe();

  ReadyOutputStreamWrapper out(*pipe.out);

  size_t totalWritten = 0;
  for (;;) {
    KJ_IF_MAYBE(n, out.write(kj::StringPtr("bar").asBytes())) {
      totalWritten += *n;
      if (*n < 3) {
        break;
      }
    } else {
      KJ_FAIL_ASSERT("pipe buffer is divisible by 3? really?");
    }
  }

  auto buf = kj::heapArray<char>(totalWritten + 1);

  // Some data will have been written to the underlying stream even though we didn't flush.
  size_t n = pipe.in->read(buf.begin(), 1, buf.size()).wait(ws);

  // We may need an explicit flush to get all the data.
  if (n < totalWritten) {
    auto flushPromise = out.flush();
    n += pipe.in->read(buf.begin() + n, totalWritten - n, buf.size() - n).wait(ws);
    flushPromise.wait(ws);
  }

  KJ_ASSERT(n == totalWritten);
  for (size_t i = 0; i < totalWritten; i++) {
    KJ_ASSERT(buf[i] == "bar"[i%3]);
  }
}

KJ_TEST("readiness IO: write even") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  auto pipe = kj::newOneWayPipe();

  ReadyOutputStreamWrapper out(*pipe.out);

  size_t totalWritten = 0;
  for (;;) {
    KJ_IF_MAYBE(n, out.write(kj::StringPtr("ba").asBytes())) {
      totalWritten += *n;
      if (*n < 2) {
        KJ_FAIL_ASSERT("pipe buffer is not divisible by 2? really?");
      }
    } else {
      break;
    }
  }

  auto buf = kj::heapArray<char>(totalWritten + 1);

  // Some data will have been written to the underlying stream even though we didn't flush.
  size_t n = pipe.in->read(buf.begin(), 1, buf.size()).wait(ws);

  // We may need an explicit flush to get all the data.
  if (n < totalWritten) {
    auto flushPromise = out.flush();
    n += pipe.in->read(buf.begin() + n, totalWritten - n, buf.size() - n).wait(ws);
    flushPromise.wait(ws);
  }

  KJ_ASSERT(n == totalWritten);
  for (size_t i = 0; i < totalWritten; i++) {
    KJ_ASSERT(buf[i] == "ba"[i%2]);
  }
}

KJ_TEST("readiness IO: read small") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  auto pipe = kj::newOneWayPipe();

  ReadyInputStreamWrapper in(*pipe.in);
  char buf[4];
  KJ_ASSERT(in.read(kj::ArrayPtr<char>(buf).asBytes()) == nullptr);

  pipe.out->writePartial("foo"_kj.asBytes()).wait(ws);

  in.whenReady().wait(ws);
  KJ_ASSERT(KJ_ASSERT_NONNULL(in.read(kj::ArrayPtr<char>(buf).asBytes())) == 3);
  buf[3] = '\0';
  KJ_ASSERT(kj::StringPtr(buf) == "foo");

  pipe.out = nullptr;

  kj::Maybe<size_t> finalRead;
  for (;;) {
    finalRead = in.read(kj::ArrayPtr<char>(buf).asBytes());
    KJ_IF_MAYBE(n, finalRead) {
      KJ_ASSERT(*n == 0);
      break;
    } else {
      in.whenReady().wait(ws);
    }
  }
}

KJ_TEST("readiness IO: read many odd") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  auto pipe = kj::newOneWayPipe();

  byte dummy[8192];
  for (auto i: kj::indices(dummy)) {
    dummy[i] = "bar"[i%3];
  }
  auto writeTask = pipe.out->writeEnd(dummy).eagerlyEvaluate(nullptr);

  ReadyInputStreamWrapper in(*pipe.in);
  char buf[3];

  for (;;) {
    auto result = in.read(kj::ArrayPtr<char>(buf).asBytes());
    KJ_IF_MAYBE(n, result) {
      for (size_t i = 0; i < *n; i++) {
        KJ_ASSERT(buf[i] == "bar"[i]);
      }
      KJ_ASSERT(*n != 0, "ended at wrong spot");
      if (*n < 3) {
        break;
      }
    } else {
      in.whenReady().wait(ws);
    }
  }

  kj::Maybe<size_t> finalRead;
  for (;;) {
    finalRead = in.read(kj::ArrayPtr<char>(buf).asBytes());
    KJ_IF_MAYBE(n, finalRead) {
      KJ_ASSERT(*n == 0);
      break;
    } else {
      in.whenReady().wait(ws);
    }
  }
}

KJ_TEST("readiness IO: read many even") {
  kj::EventLoop loop;
  kj::WaitScope ws(loop);
  auto pipe = kj::newOneWayPipe();

  byte dummy[8192];
  for (auto i: kj::indices(dummy)) {
    dummy[i] = "ba"[i%2];
  }
  auto writeTask = pipe.out->writeEnd(dummy);

  ReadyInputStreamWrapper in(*pipe.in);
  char buf[2];

  for (;;) {
    auto result = in.read(kj::ArrayPtr<char>(buf).asBytes());
    KJ_IF_MAYBE(n, result) {
      for (size_t i = 0; i < *n; i++) {
        KJ_ASSERT(buf[i] == "ba"[i]);
      }
      if (*n == 0) {
        break;
      }
      KJ_ASSERT(*n == 2, "ended at wrong spot");
    } else {
      in.whenReady().wait(ws);
    }
  }

  kj::Maybe<size_t> finalRead;
  for (;;) {
    finalRead = in.read(kj::ArrayPtr<char>(buf).asBytes());
    KJ_IF_MAYBE(n, finalRead) {
      KJ_ASSERT(*n == 0);
      break;
    } else {
      in.whenReady().wait(ws);
    }
  }
}

}  // namespace
}  // namespace kj
