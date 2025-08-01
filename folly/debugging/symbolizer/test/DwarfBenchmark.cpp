/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/Benchmark.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/Dwarf.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>
#include <folly/debugging/symbolizer/Symbolizer.h>
#include <folly/debugging/symbolizer/test/SymbolizerTestUtils.h>
#include <folly/lang/Cast.h>
#include <folly/portability/GFlags.h>

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

namespace {

using namespace folly::symbolizer;
using namespace folly::symbolizer::test;

FOLLY_NOINLINE void lexicalBlockBar() try {
  [[maybe_unused]] size_t unused = 0;
  unused++;
  inlineB_inlineA_lfind();
} catch (...) {
  folly::assume_unreachable();
}

void run(LocationInfoMode mode, size_t n) {
  folly::BenchmarkSuspender suspender;
  Symbolizer symbolizer(nullptr, LocationInfoMode::FULL_WITH_INLINE, 0);
  FrameArray<100> frames;
  gComparatorGetStackTraceArg = &frames;
  gComparatorGetStackTrace =
      folly::reinterpret_function_cast<bool(void*)>(getStackTrace<100>);
  lexicalBlockBar();
  symbolizer.symbolize(frames);
  // The address of the line where lexicalBlockBar calls inlineB_inlineA_lfind.
  uintptr_t address = frames.frames[7].addr;

  ElfCache cache;
  ElfFile elf("/proc/self/exe");
  Dwarf dwarf(&cache, &elf);
  auto inlineFrames = std::array<SymbolizedFrame, 10>();
  suspender.dismiss();

  for (size_t i = 0; i < n; i++) {
    folly::symbolizer::SymbolizedFrame frame;
    dwarf.findAddress(address, mode, frame, folly::range(inlineFrames));
  }

  suspender.rehire();
}

} // namespace

BENCHMARK(DwarfFindAddressFast, n) {
  run(folly::symbolizer::LocationInfoMode::FAST, n);
}

BENCHMARK(DwarfFindAddressFull, n) {
  run(folly::symbolizer::LocationInfoMode::FULL, n);
}

BENCHMARK(DwarfFindAddressFullWithInline, n) {
  run(folly::symbolizer::LocationInfoMode::FULL_WITH_INLINE, n);
}

#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

int main(int argc, char* argv[]) {
  folly::gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  folly::runBenchmarksOnFlag();
  return 0;
}
